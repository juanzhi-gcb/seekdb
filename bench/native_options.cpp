#include "minivectordb/database.hpp"
#include <iostream>
int main() {
  // Same aggregate initialization as the frozen score_kernel_diagnostic.cpp.
  const minivectordb::Options options{"tmp",8*1024*1024};
  std::cout<<"{\"index_cache_bytes\":"<<options.index_cache_bytes
           <<",\"data_cache_bytes\":"<<options.data_cache_bytes
           <<",\"packed_scan_cache_bytes\":"<<options.packed_scan_cache_bytes
           <<",\"scan_snapshot_bytes\":"<<options.scan_snapshot_bytes
           <<",\"memory_budget_bytes\":"<<options.memory_budget_bytes<<"}\n";
  return options.index_cache_bytes==1024*1024 && options.data_cache_bytes==4*1024*1024 ? 0 : 1;
}
