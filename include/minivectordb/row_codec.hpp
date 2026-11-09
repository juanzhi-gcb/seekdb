#pragma once
#include "minivectordb/database.hpp"
namespace minivectordb {
  enum class ColumnType:std::uint8_t {
    Bigint=1,Varchar=2,Vector=3
  };
  struct Column {
    std::string name;
    ColumnType type;
    std::uint32_t extent=0;
  };
  struct Schema {
    std::uint64_t id=0;
    std::string name;
    std::vector<Column> columns;
    std::size_t primary=0;
  };
  void validate_schema(const Schema&);
  std::string encode_row(const Schema&,const Row&);
  Row decode_row(const Schema&,std::string_view);
}
