#pragma once
#include "sql.hpp"
#include "minivectordb/store.hpp"
#include "minivectordb/row_codec.hpp"
#include "../vector/validated.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace minivectordb::sql {
// Benchmark-shaped exact serving cache: one immutable table version, clustered
// by one ordinary equality column. It owns only validated values; any mutation
// invalidates it. Unsupported shapes fall back before this class is consulted.
class ColumnStore {
public:
  struct Filter {
    bool text=false;
    std::int64_t number=0;
    std::string value;
  };
  struct SearchResult {
    std::vector<Neighbor> winners;
    std::size_t examined=0;
  };
private:
  struct TextColumn {
    std::vector<char> bytes;
    std::vector<std::uint32_t> offsets,lengths;
  };
  struct NumberRange {std::int64_t value;std::uint32_t begin,end;};
  struct TextRange {std::string_view value;std::uint32_t begin,end;};
  bool ready_=false;
  std::uint64_t table_=0;
  std::size_t primary_=0,vector_column_=0,cluster_column_=0,dimension_=0,count_=0,blocks_=0;
  std::vector<ColumnType> types_;
  std::vector<std::uint32_t> extents_;
  std::vector<std::int64_t> keys_;
  std::vector<std::vector<std::int64_t>> scalars_;
  std::vector<TextColumn> texts_;
  std::vector<float> vectors_;
  std::vector<std::uint32_t> key_order_;
  std::vector<NumberRange> number_ranges_;
  std::vector<TextRange> text_ranges_;
  std::size_t bytes_=0;

  static std::int64_t number_cell(const Cell& cell) {
    const auto* value=std::get_if<std::int64_t>(&cell);
    if(!value)throw Error(ErrorCode::Corruption,"column store expected BIGINT");
    return *value;
  }
  static const std::string& text_cell(const Cell& cell) {
    const auto* value=std::get_if<std::string>(&cell);
    if(!value)throw Error(ErrorCode::Corruption,"column store expected VARCHAR");
    return *value;
  }
  static const std::vector<float>& vector_cell(const Cell& cell) {
    const auto* value=std::get_if<std::vector<float>>(&cell);
    if(!value)throw Error(ErrorCode::Corruption,"column store expected VECTOR");
    return *value;
  }
  std::string_view text_at(std::size_t column,std::size_t row) const noexcept {
    const auto& text=texts_[column];
    return {text.bytes.data()+text.offsets[row],text.lengths[row]};
  }
public:
  void clear() noexcept {
    ready_=false;table_=0;primary_=vector_column_=cluster_column_=dimension_=count_=blocks_=0;
    types_.clear();extents_.clear();keys_.clear();scalars_.clear();texts_.clear();
    vectors_.clear();key_order_.clear();number_ranges_.clear();text_ranges_.clear();bytes_=0;
  }
  bool ready(const Schema& schema,std::size_t vector_column,std::size_t cluster_column) const noexcept {
    return matches_table(schema,vector_column)&&cluster_column_==cluster_column;
  }
  bool matches_table(const Schema& schema,std::size_t vector_column) const noexcept {
    if(!ready_||table_!=schema.id||vector_column_!=vector_column||primary_!=schema.primary||
       types_.size()!=schema.columns.size())return false;
    for(std::size_t c=0;c<schema.columns.size();++c)
      if(types_[c]!=schema.columns[c].type||extents_[c]!=schema.columns[c].extent)return false;
    return true;
  }
  std::size_t cluster_column() const noexcept {return cluster_column_;}
  std::size_t count() const noexcept {return count_;}
  std::size_t bytes() const noexcept {return bytes_;}
  std::optional<std::size_t> row_for_key(std::int64_t value) const noexcept {
    if(!ready_||key_order_.empty())return {};
    const auto begin=key_order_.data(),end=begin+key_order_.size();
    const auto at=std::lower_bound(begin,end,value,[&](std::uint32_t row,std::int64_t wanted){return keys_[row]<wanted;});
    if(at==end||keys_[*at]!=value)return {};
    return *at;
  }
  bool build(const Store& store,const Schema& schema,std::size_t vector_column,
             std::size_t cluster_column,std::size_t budget_bytes) {
    clear();
    if(!budget_bytes||vector_column>=schema.columns.size()||cluster_column>=schema.columns.size()||
       vector_column==cluster_column||schema.columns[vector_column].type!=ColumnType::Vector||
       schema.columns[cluster_column].type==ColumnType::Vector||
       schema.columns[schema.primary].type!=ColumnType::Bigint||schema.columns[vector_column].extent==0)return false;
    const std::size_t dimension=schema.columns[vector_column].extent;
    std::size_t scalar_count=0,text_count=0;
    constexpr std::size_t no_slot=std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> text_slots(schema.columns.size(),no_slot);
    for(std::size_t c=0;c<schema.columns.size();++c) {
      if(schema.columns[c].type==ColumnType::Bigint)++scalar_count;
      else if(schema.columns[c].type==ColumnType::Varchar)text_slots[c]=text_count++;
    }
    std::vector<std::int64_t> keys;
    std::vector<std::int64_t> cluster_numbers;
    std::vector<std::string> cluster_texts;
    std::vector<std::uint32_t> text_lengths;
    std::size_t text_bytes=0,estimated=0;
    bool over_budget=false;
    try {
      store.scan_in(schema.id,[&](std::int64_t scan_key,std::string_view bytes) {
        if(over_budget)return;
        const Row row=decode_row(schema,bytes);
        if(number_cell(row[schema.primary])!=scan_key)
          throw Error(ErrorCode::Corruption,"column store primary key disagrees with index");
        std::size_t row_text=0;
        for(std::size_t c=0;c<schema.columns.size();++c)if(text_slots[c]!=no_slot) {
          const auto& text=text_cell(row[c]);
          text_lengths.push_back(static_cast<std::uint32_t>(text.size()));
          row_text+=text.size();
        }
        if(schema.columns[cluster_column].type==ColumnType::Bigint)
          cluster_numbers.push_back(number_cell(row[cluster_column]));
        else cluster_texts.push_back(text_cell(row[cluster_column]));
        keys.push_back(scan_key);
        text_bytes+=row_text;
        estimated+=sizeof(std::int64_t)+dimension*sizeof(float)+scalar_count*sizeof(std::int64_t)+
          text_count*(sizeof(std::uint32_t)*2)+row_text;
        if(estimated>budget_bytes)over_budget=true;
      });
    } catch(...) {clear();throw;}
    if(over_budget||keys.empty()) {clear();return false;}
    const std::size_t n=keys.size();
    std::vector<std::uint32_t> order(n);
    std::iota(order.begin(),order.end(),0U);
    if(schema.columns[cluster_column].type==ColumnType::Bigint) {
      std::sort(order.begin(),order.end(),[&](auto a,auto b) {
        return cluster_numbers[a]<cluster_numbers[b]||
          (cluster_numbers[a]==cluster_numbers[b]&&keys[a]<keys[b]);
      });
    } else {
      std::sort(order.begin(),order.end(),[&](auto a,auto b) {
        const int cmp=cluster_texts[a].compare(cluster_texts[b]);
        return cmp<0||(cmp==0&&keys[a]<keys[b]);
      });
    }
    std::unordered_map<std::int64_t,std::uint32_t> destinations;
    destinations.reserve(n*2);
    for(std::size_t row=0;row<n;++row)destinations.emplace(keys[order[row]],static_cast<std::uint32_t>(row));
    if(destinations.size()!=n) {clear();return false;}
    const std::size_t blocks=(n+15)/16;
    const std::size_t scalar_bytes=n*scalar_count*sizeof(std::int64_t);
    std::size_t text_meta=0,text_total=0;
    for(std::size_t c=0;c<schema.columns.size();++c)if(text_slots[c]!=no_slot) {
      text_meta+=n*(sizeof(std::uint32_t)*2);
      for(std::size_t row=0;row<n;++row)text_total+=text_lengths[order[row]*text_count+text_slots[c]];
    }
    const std::size_t total=n*sizeof(std::int64_t)+n*sizeof(std::uint32_t)+
      blocks*16*dimension*sizeof(float)+scalar_bytes+text_meta+text_total;
    if(total>budget_bytes) {clear();return false;}
    try {
      table_=schema.id;primary_=schema.primary;vector_column_=vector_column;
      cluster_column_=cluster_column;dimension_=dimension;count_=n;blocks_=blocks;
      types_.resize(schema.columns.size());extents_.resize(schema.columns.size());
      for(std::size_t c=0;c<schema.columns.size();++c) {
        types_[c]=schema.columns[c].type;extents_[c]=schema.columns[c].extent;
      }
      keys_.resize(n);vectors_.assign(blocks*16*dimension,0.0f);
      scalars_.resize(schema.columns.size());
      texts_.resize(schema.columns.size());
      for(std::size_t c=0;c<schema.columns.size();++c) {
        if(schema.columns[c].type==ColumnType::Bigint)scalars_[c].resize(n);
        else if(schema.columns[c].type==ColumnType::Varchar) {
          auto& text=texts_[c];
          text.offsets.resize(n);text.lengths.resize(n);
          std::uint32_t offset=0;
          for(std::size_t row=0;row<n;++row) {
            const auto length=text_lengths[order[row]*text_count+text_slots[c]];
            text.offsets[row]=offset;text.lengths[row]=length;offset+=length;
          }
          text.bytes.resize(offset);
        }
      }
      std::vector<unsigned char> filled(n,0);
      std::size_t seen=0;
      store.scan_in(schema.id,[&](std::int64_t scan_key,std::string_view bytes) {
        const Row row=decode_row(schema,bytes);
        const auto found=destinations.find(scan_key);
        if(found==destinations.end())throw Error(ErrorCode::Corruption,"column store key set changed");
        const auto destination=found->second;
        if(filled[destination])throw Error(ErrorCode::Corruption,"column store duplicate key");
        filled[destination]=1;keys_[destination]=scan_key;++seen;
        for(std::size_t c=0;c<schema.columns.size();++c) {
          if(schema.columns[c].type==ColumnType::Bigint)scalars_[c][destination]=number_cell(row[c]);
          else if(schema.columns[c].type==ColumnType::Varchar) {
            const auto& value=text_cell(row[c]);
            auto& text=texts_[c];
            if(value.size()!=text.lengths[destination])
              throw Error(ErrorCode::Corruption,"column store text length changed");
            if(!value.empty())std::memcpy(text.bytes.data()+text.offsets[destination],value.data(),value.size());
          }
        }
        const auto& vector=vector_cell(row[vector_column]);
        if(vector.size()!=dimension)throw Error(ErrorCode::Corruption,"column store vector dimension changed");
        const std::size_t block=destination/16,lane=destination%16;
        auto* values=vectors_.data()+block*16*dimension;
        for(std::size_t d=0;d<dimension;++d)values[d*16+lane]=vector[d];
      });
      if(seen!=n)throw Error(ErrorCode::Corruption,"column store row count changed");
      key_order_.resize(n);std::iota(key_order_.begin(),key_order_.end(),0U);
      std::sort(key_order_.begin(),key_order_.end(),[&](auto a,auto b){return keys_[a]<keys_[b];});
      number_ranges_.clear();text_ranges_.clear();
      std::size_t begin=0;
      while(begin<n) {
        std::size_t end=begin+1;
        if(schema.columns[cluster_column].type==ColumnType::Bigint) {
          const auto value=scalars_[cluster_column][begin];
          while(end<n&&scalars_[cluster_column][end]==value)++end;
          number_ranges_.push_back({value,static_cast<std::uint32_t>(begin),static_cast<std::uint32_t>(end)});
        } else {
          const auto value=text_at(cluster_column,begin);
          while(end<n&&text_at(cluster_column,end)==value)++end;
          text_ranges_.push_back({value,static_cast<std::uint32_t>(begin),static_cast<std::uint32_t>(end)});
        }
        begin=end;
      }
      bytes_=keys_.capacity()*sizeof(std::int64_t)+key_order_.capacity()*sizeof(std::uint32_t)+
        vectors_.capacity()*sizeof(float);
      for(const auto& scalar:scalars_)bytes_+=scalar.capacity()*sizeof(std::int64_t);
      for(const auto& text:texts_)bytes_+=text.bytes.capacity()+text.offsets.capacity()*sizeof(std::uint32_t)+
        text.lengths.capacity()*sizeof(std::uint32_t);
      bytes_+=number_ranges_.capacity()*sizeof(NumberRange)+text_ranges_.capacity()*sizeof(TextRange);
      ready_=true;
      return true;
    } catch(...) {clear();throw;}
  }
  std::optional<SearchResult> search(std::span<const float> query,std::size_t k,
      const std::optional<Filter>& filter,bool allow_wide,bool allow_lower_bound) const {
    if(!ready_||!k)return std::nullopt;
    std::size_t begin=0,end=count_;
    if(filter) {
      if(filter->text) {
        if(types_[cluster_column_]!=ColumnType::Varchar)return std::nullopt;
        const auto at=std::lower_bound(text_ranges_.begin(),text_ranges_.end(),std::string_view(filter->value),
          [](const TextRange& range,std::string_view value){return range.value<value;});
        if(at==text_ranges_.end()||at->value!=filter->value)return SearchResult{};
        begin=at->begin;end=at->end;
      } else {
        if(types_[cluster_column_]!=ColumnType::Bigint)return std::nullopt;
        const auto at=std::lower_bound(number_ranges_.begin(),number_ranges_.end(),filter->number,
          [](const NumberRange& range,std::int64_t value){return range.value<value;});
        if(at==number_ranges_.end()||at->value!=filter->number)return SearchResult{};
        begin=at->begin;end=at->end;
      }
    }
    if(begin>=end)return SearchResult{};
    try {
      auto winners=detail::exact_top_k_transposed_validated_candidates(query,k,
        [&](const detail::BlockVectorVisitor& visit) {
          std::array<std::int64_t,16> block_keys{};
          for(std::size_t block=begin/16;block*16<end;++block) {
            const std::size_t base=block*16;
            const std::size_t lo=std::max(begin,base),hi=std::min(end,base+16);
            std::uint16_t mask=0;
            for(std::size_t row=lo;row<hi;++row) {
              block_keys[row-base]=keys_[row];
              mask|=static_cast<std::uint16_t>(1U<<(row-base));
            }
            visit({block_keys.data(),16},{vectors_.data()+base*dimension_,16*dimension_},mask);
          }
        },allow_wide,allow_lower_bound,nullptr,nullptr);
      return SearchResult{std::move(winners),end-begin};
    } catch(const std::bad_alloc&) {return std::nullopt;}
  }
  bool project(const Schema& schema,const Bound& bound,std::size_t row,double distance,Row& out) const {
    if(!ready_||row>=count_)return false;
    out.clear();out.reserve(bound.projection.size());
    for(const auto& projection:bound.projection) {
      if(projection.query) {
        if(projection.column!=vector_column_||!projection.query||projection.query->size()!=dimension_)return false;
        out.emplace_back(distance);
      } else if(projection.column>=schema.columns.size())return false;
      else if(schema.columns[projection.column].type==ColumnType::Bigint)
        out.emplace_back(scalars_[projection.column][row]);
      else if(schema.columns[projection.column].type==ColumnType::Varchar)
        out.emplace_back(std::string(text_at(projection.column,row)));
      else return false;
    }
    return true;
  }
};
}
