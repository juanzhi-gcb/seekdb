#include "minivectordb/row_codec.hpp"
#include "minivectordb/value.hpp"
#include "binary.hpp"
#include <bit>
#include <cmath>
#include <set>
namespace minivectordb {
  void validate_schema(const Schema&s) {
    if(s.name.empty()||s.columns.empty()||s.columns.size()>UINT32_MAX||s.primary>=s.columns.size()||s.columns[s.primary].type!=ColumnType::Bigint)throw Error(ErrorCode::Schema,"table requires exactly one BIGINT primary key");
    std::set<std::string> names;
    unsigned vectors=0;
    for(const auto&c:s.columns) {
      if(c.name.empty()||!names.insert(c.name).second)throw Error(ErrorCode::Schema,"duplicate or empty column name");
      if(c.type==ColumnType::Vector)++vectors;
      if((c.type==ColumnType::Vector||c.type==ColumnType::Varchar)&&!c.extent)throw Error(ErrorCode::Schema,"type extent must be positive");
      if(c.type!=ColumnType::Bigint&&c.type!=ColumnType::Varchar&&c.type!=ColumnType::Vector)throw Error(ErrorCode::Schema,"unknown column type");
    }
    if(vectors>1)throw Error(ErrorCode::Schema,"at most one VECTOR column supported");
  }
  std::string encode_row(const Schema&s,const Row&r) {
    if(r.size()!=s.columns.size())throw Error(ErrorCode::Type,"row column count mismatch");
    std::string out;
    binary::put(out,1,1);
    binary::put(out,r.size(),4);
    for(std::size_t i=0;i<r.size();++i) {
      const auto&c=s.columns[i];
      const auto&v=r[i];
      try {
        switch(c.type) {
          case ColumnType::Bigint:if(!std::holds_alternative<std::int64_t>(v))throw Error(ErrorCode::Type,"expected BIGINT");
          binary::put(out,std::bit_cast<std::uint64_t>(std::get<std::int64_t>(v)));
          break;
          case ColumnType::Varchar:if(!std::holds_alternative<std::string>(v))throw Error(ErrorCode::Type,"expected VARCHAR");
          validate_text(std::get<std::string>(v),c.extent);
          binary::text(out,std::get<std::string>(v));
          break;
          case ColumnType::Vector: {
            if(!std::holds_alternative<std::vector<float>>(v))throw Error(ErrorCode::Type,"expected VECTOR");
            const auto&a=std::get<std::vector<float>>(v);
            if(a.size()!=c.extent)throw Error(ErrorCode::Type,"VECTOR dimension mismatch");
            binary::put(out,a.size(),4);
            for(float f:a) {
              if(!std::isfinite(f))throw Error(ErrorCode::Type,"non-finite VECTOR component");
              binary::put(out,std::bit_cast<std::uint32_t>(f),4);
            }
            break;
          }
        }
      }
      catch(const std::invalid_argument&e) {
        throw Error(ErrorCode::Type,e.what());
      }
    }
    return out;
  }
  Row decode_row(const Schema&s,std::string_view bytes) {
    binary::Reader in(bytes);
    if(in.get(1)!=1||in.get(4)!=s.columns.size())throw Error(ErrorCode::Corruption,"row version or column count mismatch");
    Row r;
    r.reserve(s.columns.size());
    for(const auto&c:s.columns) {
      switch(c.type) {
        case ColumnType::Bigint:r.emplace_back(std::bit_cast<std::int64_t>(in.get()));
        break;
        case ColumnType::Varchar: {
          auto text=in.text();
          try {
            validate_text(text,c.extent);
          }
          catch(const std::invalid_argument&e) {
            throw Error(ErrorCode::Corruption,e.what());
          }
          r.emplace_back(std::move(text));
          break;
        }
        case ColumnType::Vector: {
          auto n=in.get(4);
          if(n!=c.extent||n>in.remaining()/4)throw Error(ErrorCode::Corruption,"invalid VECTOR length");
          std::vector<float> v;
          v.reserve(static_cast<std::size_t>(n));
          for(std::uint64_t i=0;i<n;++i) {
            float f=std::bit_cast<float>(static_cast<std::uint32_t>(in.get(4)));
            if(!std::isfinite(f))throw Error(ErrorCode::Corruption,"non-finite stored VECTOR");
            v.push_back(f);
          }
          r.emplace_back(std::move(v));
          break;
        }
      }
    }
    in.end();
    return r;
  }
}
