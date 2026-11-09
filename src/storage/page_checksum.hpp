#pragma once
#include <array>
#include "minivectordb/profile.hpp"
#include <cstddef>
#include <cstdint>

namespace minivectordb::detail {
// Reflected IEEE CRC-32 used by format 02 (not the CRC-32C of x86 CRC32).
inline constexpr auto page_crc32_table=[] {
  std::array<std::uint32_t,256> table{};
  for(std::size_t i=0;i<table.size();++i) {
    auto crc=static_cast<std::uint32_t>(i);
    for(unsigned bit=0;bit<8;++bit)
      crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));
    table[i]=crc;
  }
  return table;
}();

// The four checksum bytes are treated as zero, without modifying the page.
inline std::uint32_t page_checksum(const std::array<unsigned char,4096>& page,
                                   std::size_t checksum_offset) {
  profile::detail::Scope timer(&profile::Snapshot::page_checksum);
  std::uint32_t crc=0xffffffffu;
  for(std::size_t i=0;i<page.size();++i) {
    const auto byte=(i>=checksum_offset && i<checksum_offset+4)?0u:page[i];
    crc=(crc>>8)^page_crc32_table[(crc^byte)&0xffu];
  }
  return ~crc;
}
}
