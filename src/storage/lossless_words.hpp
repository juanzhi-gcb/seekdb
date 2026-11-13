#pragma once
#include "../vector/lossless_half.hpp"
#include <algorithm>
#include <stdexcept>
#include <bit>
#include <cstring>
#include <string>
namespace minivectordb::detail::halfwords {
inline float word(const char*p){float x;std::memcpy(&x,p,4);return x;}
inline bool transform(std::string_view s,std::string& out){
 unsigned align=0;std::size_t best=0;
 for(unsigned a=0;a<4&&a<=s.size();++a){std::size_t count=0;const auto words=(s.size()-a)/4;const auto samples=std::min(words,std::size_t(16));for(std::size_t i=0;i<samples;++i){const auto p=a+4*(i*words/samples);count+=minivectordb::detail::half_exact(word(s.data()+p));}if(count>best){align=a;best=count;}}
 // General-purpose lossless bit pattern admission, not a SQL/vector assumption.
 if(best<4)return false;
 out.clear();out.reserve(s.size());out+=char(align);out.append(s.substr(0,align));std::size_t p=align;
 while(p+4<=s.size()){const auto n=std::min(std::size_t(32),(s.size()-p)/4);std::uint32_t mask=0;for(unsigned i=0;i<n;++i)if(minivectordb::detail::half_exact(word(s.data()+p+i*4)))mask|=1U<<i;out.append(reinterpret_cast<const char*>(&mask),4);
  for(unsigned i=0;i<n;++i){if(mask&(1U<<i)){auto h=minivectordb::detail::half_encode_exact(word(s.data()+p+i*4));out.append(reinterpret_cast<const char*>(&h),2);}else out.append(s.substr(p+i*4,4));}p+=n*4;
 }out.append(s.substr(p));return out.size()+8<s.size();
}
inline std::string inverse(std::string_view s,std::size_t expected){
 auto bad=[](){throw std::runtime_error("invalid compact float words");};if(s.empty()||expected>4064)bad();const auto align=static_cast<unsigned char>(s[0]);if(align>3||align>expected||s.size()<1+align)bad();std::string out(s.substr(1,align));out.reserve(expected);std::size_t p=1+align;
 while(out.size()+4<=expected){auto n=std::min(std::size_t(32),(expected-out.size())/4);if(s.size()-p<4)bad();std::uint32_t mask;std::memcpy(&mask,s.data()+p,4);p+=4;if(n<32&&(mask>>n))bad();
  for(unsigned i=0;i<n;++i){if(mask&(1U<<i)){if(s.size()-p<2)bad();std::uint16_t h;std::memcpy(&h,s.data()+p,2);p+=2;if((h&0x7c00U)==0x7c00U)bad();float x=minivectordb::detail::half_decode(h);out.append(reinterpret_cast<const char*>(&x),4);}else{if(s.size()-p<4)bad();out.append(s.substr(p,4));p+=4;}}
 }if(s.size()-p!=expected-out.size())bad();out.append(s.substr(p));return out;
}
}
