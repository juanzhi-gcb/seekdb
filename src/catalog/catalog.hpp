#pragma once
#include "minivectordb/row_codec.hpp"
#include "minivectordb/store.hpp"
namespace minivectordb {
  Schema lookup_table(const Store&,std::string_view);
  void create_table(Store&,Schema);
}
