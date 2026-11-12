#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <stdexcept>
namespace minivectordb::detail {
// Bounded lossless byte runs, independent of SQL/types/workload contents.
// 4-byte decoded size; control <128 = 1..128 literals, else 3..130 repeats.
inline bool pack_bytes(std::string_view input,std::string& output) {
  output.clear();if(input.size()<64 || input.size()>4064)return false;
  output.reserve(input.size()+input.size()/128+8);
  for(unsigned k=0;k<4;++k)output.push_back(static_cast<char>(input.size()>>(k*8)));
  std::size_t i=0;
  while(i<input.size()) {
    auto end=i+1;
    while(end<input.size() && end-i<130 && input[end]==input[i])++end;
    if(end-i>=3) {
      output.push_back(static_cast<char>(128+end-i-3));output.push_back(input[i]);i=end;
    } else {
      const auto begin=i;i=end;
      while(i<input.size() && i-begin<128) {
        end=i+1;while(end<input.size() && end-i<3 && input[end]==input[i])++end;
        if(end-i>=3)break;
        i=std::min(end,begin+128);
      }
      output.push_back(static_cast<char>(i-begin-1));output.append(input.substr(begin,i-begin));
    }
  }
  return output.size()+8<=input.size();
}
inline std::string unpack_bytes(std::string_view input,std::size_t maximum=4064) {
  auto bad=[](){throw std::runtime_error("corrupt MiniVectorDB packed value");};
  if(input.size()<4)bad();
  std::size_t expected=0;
  for(unsigned k=0;k<4;++k)expected|=std::size_t(static_cast<unsigned char>(input[k]))<<(k*8);
  if(expected<64 || expected>maximum || input.size()+8>expected)bad();
  std::string output;output.reserve(expected);std::size_t i=4;
  while(i<input.size()) {
    const auto control=static_cast<unsigned char>(input[i++]);
    const std::size_t length=control<128?std::size_t(control)+1:std::size_t(control)-128+3;
    if(length>expected-output.size())bad();
    if(control<128) {
      if(length>input.size()-i)bad();
      output.append(input.substr(i,length));i+=length;
    } else {
      if(i==input.size())bad();
      output.append(length,input[i++]);
    }
  }
  if(output.size()!=expected)bad();
  return output;
}
}
