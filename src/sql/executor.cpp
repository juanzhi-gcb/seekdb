#include "sql.hpp"
#include "sorter.hpp"
#include "../catalog/catalog.hpp"
#include "minivectordb/exact.hpp"
#include <cmath>
#include <limits>
namespace minivectordb {
  namespace {
    struct StopScan {
    };
    void count_decoded(const Row& row,ExecutionInfo& info) {
      info.cells_materialized+=row.size();
      for(const auto& cell:row)if(std::holds_alternative<std::vector<float>>(cell))++info.vector_values_materialized;
    }
    class MutationBatch {
      Store& store_;
      std::uint64_t space_;
      bool enabled_;
      std::size_t bytes_=0;
      std::vector<Store::Mutation> mutations_;
    public:
      MutationBatch(Store& store,std::uint64_t space,bool enabled):store_(store),space_(space),enabled_(enabled) {}
      void flush() {
        if(mutations_.empty())return;
        store_.apply_batch_in(space_,mutations_);
        mutations_.clear(); bytes_=0;
      }
      void add(std::int64_t key,std::optional<std::string> value) {
        if(!enabled_) {
          if(value)store_.put_in(space_,key,*value);
          else store_.erase_in(space_,key);
          return;
        }
        const auto size=value?value->size():0;
        if(!mutations_.empty() && (mutations_.size()==Store::max_batch_mutations ||
            size>Store::max_batch_bytes-bytes_))flush();
        mutations_.push_back({key,std::move(value)});
        bytes_+=size;
        if(bytes_>=Store::max_batch_bytes)flush();
      }
    };
    class SelectRow {
      Row eager_;
      std::optional<LazyRowView> lazy_;
    public:
      SelectRow(const Schema& schema,std::int64_t key,std::string_view bytes,bool selective,ExecutionInfo& info) {
        if(selective)lazy_.emplace(schema,bytes,info);
        else { eager_=decode_row(schema,bytes); count_decoded(eager_,info); }
        if(std::get<std::int64_t>((*this)[schema.primary])!=key)
          throw Error(ErrorCode::Corruption,"row primary key disagrees with index");
      }
      const Cell& operator[](std::size_t index) const { return lazy_?(*lazy_)[index]:eager_[index]; }
      bool matches(const std::vector<sql::BoundPredicate>& predicates) const {
        for(const auto& p:predicates) {
          int cmp=sql::compare_cells((*this)[p.column],p.value);
          if((p.op=="="&&cmp!=0)||(p.op=="<"&&cmp>=0)||(p.op=="<="&&cmp>0)||(p.op==">"&&cmp<=0)||(p.op==">="&&cmp<0))return false;
        }
        return true;
      }
    };
    struct DistanceCache {
      std::vector<std::pair<const sql::BoundExpr*,double>> values;
      bool same(const sql::BoundExpr&a,const sql::BoundExpr&b)const {
        return a.column==b.column&&a.query==b.query;
      }
      double square(const sql::BoundExpr&e,const SelectRow&r,ExecutionInfo&info) {
        for(const auto&[expr,value]:values)if(same(*expr,e))return value;
        auto value=sql::squared_distance(std::get<std::vector<float>>(r[e.column]),*e.query);
        ++info.distances_computed;
        values.emplace_back(&e,value);
        return value;
      }
    };
    Row project(const sql::Bound&b,const SelectRow&r,DistanceCache&cache,ExecutionInfo&info) {
      Row out;
      out.reserve(b.projection.size());
      for(const auto&e:b.projection) {
        if(e.query)out.emplace_back(std::sqrt(cache.square(e,r,info)));
        else out.push_back(r[e.column]);
      }
      return out;
    }
  }
  struct Database::Impl {
    Store store;
    Options options;
    bool active=false,poisoned=false,busy=false;
    Impl(const std::filesystem::path&path,Options settings):store(path),options(std::move(settings)) {
      if(options.sort_directory.empty())options.sort_directory=path.parent_path()/"tmp";
      if(options.memory_budget_bytes<4096)throw Error(ErrorCode::Constraint,"sort memory budget must be at least 4096 bytes");
    }
    ~Impl() {
      if(active&&!poisoned) {
        try {
          store.rollback();
        }
        catch(...) {
        }
      }
    }
    Row decode(const Schema&s,std::int64_t key,std::string_view bytes) {
      auto row=decode_row(s,bytes);
      if(std::get<std::int64_t>(row[s.primary])!=key)throw Error(ErrorCode::Corruption,"row primary key disagrees with index");
      return row;
    }
    void scan(const Schema&s,const sql::Bound&b,ExecutionInfo&info,const std::function<void(std::int64_t,const Row&)>&visit) {
      auto callback=[&](auto key,auto bytes) {
        ++info.rows_examined;
        auto row=decode(s,key,bytes);
        count_decoded(row,info);
        if(sql::matches(row,b.predicates))visit(key,row);
      };
      if(b.primary_lookup) {
        if(auto raw=store.get_in(s.id,*b.primary_lookup))callback(*b.primary_lookup,*raw);
      }
      else store.scan_in(s.id,callback);
    }
    void scan_select(const Schema& s,const sql::Bound& b,ExecutionInfo& info,
                     const std::function<void(std::int64_t,const SelectRow&)>& visit) {
      auto callback=[&](auto key,auto bytes) {
        ++info.rows_examined;
        SelectRow row(s,key,bytes,options.selective_decode,info);
        if(row.matches(b.predicates))visit(key,row);
      };
      if(b.primary_lookup) {
        if(auto raw=store.get_in(s.id,*b.primary_lookup))callback(*b.primary_lookup,*raw);
      } else store.scan_in(s.id,callback);
    }
    void fetch_select(const Schema& s,std::int64_t key,ExecutionInfo& info,
                      const std::function<void(const SelectRow&)>& visit) {
      auto raw=store.get_in(s.id,key);
      if(!raw)throw Error(ErrorCode::Corruption,"selected row disappeared");
      SelectRow row(s,key,*raw,options.selective_decode,info);
      visit(row);
    }
    ExecutionInfo select(const sql::Statement&statement,const Schema&s,const sql::Bound&b,const QuerySink&sink) {
      ExecutionInfo info;
      std::vector<std::string> labels;
      for(const auto&e:b.projection)labels.push_back(e.label);
      if(sink.columns)sink.columns(labels);
      auto limit=statement.limit.value_or(UINT64_MAX);
      if(!limit)return info;
      auto emit=[&](const SelectRow&r,DistanceCache&cache) {
        auto output=project(b,r,cache,info);
        if(sink.row)sink.row(output);
        ++info.affected_rows;
      };
      if(b.order.empty()) {
        try {
          scan_select(s,b,info,[&](auto,const SelectRow&r) {
            DistanceCache cache;
            emit(r,cache);
            if(--limit==0)throw StopScan {
            };
          });
        }
        catch(const StopScan&) {
        }
        return info;
      }
      if(b.order.size()==1&&!b.order[0].descending&&b.order[0].expression.query&&statement.limit&&limit<=options.memory_budget_bytes/(4*sizeof(Neighbor))) {
        const auto&e=b.order[0].expression;
        auto winners=exact_top_k(*e.query,static_cast<std::size_t>(limit),[&](const VectorVisitor&visit) {
          scan_select(s,b,info,[&](auto key,const SelectRow&r) {
            ++info.distances_computed;
            visit(key,std::get<std::vector<float>>(r[e.column]));
          });
        });
        for(const auto&winner:winners) {
          fetch_select(s,winner.key,info,[&](const SelectRow& row) {
            DistanceCache cache;
            Row output;
            output.reserve(b.projection.size());
            for(const auto&projection:b.projection) {
              if(projection.query&&cache.same(projection,e))output.emplace_back(winner.distance);
              else if(projection.query)output.emplace_back(std::sqrt(cache.square(projection,row,info)));
              else output.push_back(row[projection.column]);
            }
            if(sink.row)sink.row(output);
            ++info.affected_rows;
          });
        }
        return info;
      }
      sql::Sorter sorter(options,b.order,info);
      scan_select(s,b,info,[&](auto key,const SelectRow&r) {
        DistanceCache cache;
        sql::SortRecord record;
        record.key=key;
        record.values.reserve(b.order.size());
        for(const auto&o:b.order) {
          if(o.expression.query)record.values.emplace_back(cache.square(o.expression,r,info));
          else record.values.push_back(r[o.expression.column]);
        }
        sorter.add(std::move(record));
      });
      sorter.finish(limit,[&](const sql::SortRecord&record) {
        fetch_select(s,record.key,info,[&](const SelectRow& row) {
          DistanceCache cache;
          for(std::size_t i=0;i<b.order.size();++i)if(b.order[i].expression.query)cache.values.emplace_back(&b.order[i].expression,std::get<double>(record.values[i]));
          emit(row,cache);
        });
      });
      return info;
    }
    ExecutionInfo execute(std::string_view text,const QuerySink&sink) {
      if(busy)throw Error(ErrorCode::Transaction,"Database callbacks cannot reenter execute");
      if(poisoned)throw Error(ErrorCode::Transaction,"commit outcome uncertain; close and reopen Database");
      busy=true;
      struct Reset {
        bool&flag;
        ~Reset() {
          flag=false;
        }
      }
      reset {
        busy
      };
      auto statement=sql::parse(text);
      ExecutionInfo info;
      if(statement.kind==sql::Statement::Begin) {
        if(active)throw Error(ErrorCode::Transaction,"transaction already active");
        store.begin();
        active=true;
        return info;
      }
      if(statement.kind==sql::Statement::Commit) {
        if(!active)throw Error(ErrorCode::Transaction,"no active transaction");
        try {
          store.commit();
        }
        catch(...) {
          poisoned=true;
          throw;
        }
        active=false;
        return info;
      }
      if(statement.kind==sql::Statement::Rollback) {
        if(!active)throw Error(ErrorCode::Transaction,"no active transaction");
        store.rollback();
        active=false;
        return info;
      }
      Schema schema;
      sql::Bound bound;
      if(statement.kind!=sql::Statement::Create) {
        schema=lookup_table(store,statement.table);
        bound=sql::bind(statement,schema);
      }
      if(statement.kind==sql::Statement::Select)return select(statement,schema,bound,sink);
      // Binding and existing-key checks finish before any statement mutation.
      if(statement.kind==sql::Statement::Insert)for(const auto&r:bound.insert_rows)if(store.get_in(schema.id,std::get<std::int64_t>(r[schema.primary])))throw Error(ErrorCode::Constraint,"duplicate primary key");
      const bool automatic=!active;
      if(automatic) {
        store.begin();
        active=true;
      }
      const auto savepoint=store.savepoint();
      try {
        MutationBatch batch(store,schema.id,options.batch_writes);
        if(statement.kind==sql::Statement::Create)create_table(store,statement.schema);
        else if(statement.kind==sql::Statement::Insert) {
          for(const auto&r:bound.insert_rows) {
            batch.add(std::get<std::int64_t>(r[schema.primary]),encode_row(schema,r));
            ++info.affected_rows;
          }
        }
        else if(statement.kind==sql::Statement::Update) {
          scan(schema,bound,info,[&](auto key,const Row&old) {
            auto r=old;
            for(const auto&[column,value]:bound.assignments)r[column]=value;
            batch.add(key,encode_row(schema,r));
            ++info.affected_rows;
          });
        }
        else if(statement.kind==sql::Statement::Delete) {
          scan(schema,bound,info,[&](auto key,const Row&) {
            batch.add(key,std::nullopt);
            ++info.affected_rows;
          });
        }
        batch.flush();
      }
      catch(...) {
        store.restore(savepoint);
        if(automatic) {
          store.rollback();
          active=false;
        }
        throw;
      }
      // Never restore/rollback after a commit error: root publication may already be durable.
      if(automatic) {
        try {
          store.commit();
        }
        catch(...) {
          poisoned=true;
          throw;
        }
        active=false;
      }
      return info;
    }
  };
  Database::Database(const std::filesystem::path&path,Options options):impl_(std::make_unique<Impl>(path,std::move(options))) {
  }
  Database::~Database()=default;
  ExecutionInfo Database::execute(std::string_view sql,const QuerySink&sink) {
    return impl_->execute(sql,sink);
  }
  bool Database::in_transaction()const noexcept {
    return impl_->active;
  }
}
