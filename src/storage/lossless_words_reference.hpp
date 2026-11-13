#pragma once
#include "../vector/lossless_half.hpp"
#include <algorithm>
#include <stdexcept>
#include <bit>
#include <cstring>
#include <string>
#include <array>
#include <cmath>
namespace minivectordb::detail::halfwords_reference {
inline float word(const char*p){float x;std::memcpy(&x,p,4);return x;}
inline int choose_alignment(std::string_view s){
 unsigned align=0;std::size_t best=0;int best_balance=0;
 for(unsigned a=0;a<4&&a<=s.size();++a){
  std::size_t count=0;int balance=0;std::uint32_t tag=0;
  const auto words=(s.size()-a)/4,samples=std::min(words,std::size_t(16));
  for(std::size_t i=0;i<samples;++i){const auto p=a+4*(i*words/samples);const float value=word(s.data()+p);
   if(minivectordb::detail::half_exact(value)){
    ++count;const auto bits=std::bit_cast<std::uint32_t>(value);
    if(!balance){tag=bits;balance=1;}else balance+=bits==tag?1:-1;
   }
  }
  if(count>best){align=a;best=count;best_balance=balance;}
 }
 // A dominant repeated word is already well handled by byte-plane runs.
 // This is a compression-selection heuristic only; all encodings remain exact.
 if(best<4 || best_balance>int(best/2))return -1;
 return int(align);
}
inline bool transform_half(std::string_view s,std::string& out){
 const int chosen=choose_alignment(s);if(chosen<0)return false;const unsigned align=unsigned(chosen);
 out.clear();out.reserve(s.size());out+=char(align);out.append(s.substr(0,align));std::size_t p=align;
 while(p+4<=s.size()){const auto n=std::min(std::size_t(32),(s.size()-p)/4);std::uint32_t mask=0;for(unsigned i=0;i<n;++i)if(minivectordb::detail::half_exact(word(s.data()+p+i*4)))mask|=1U<<i;out.append(reinterpret_cast<const char*>(&mask),4);
  for(unsigned i=0;i<n;++i){if(mask&(1U<<i)){auto h=minivectordb::detail::half_encode_exact(word(s.data()+p+i*4));out.append(reinterpret_cast<const char*>(&h),2);}else out.append(s.substr(p+i*4,4));}p+=n*4;
 }out.append(s.substr(p));return out.size()+8<s.size();
}
inline std::string inverse_half(std::string_view s,std::size_t expected){
 auto bad=[](){throw std::runtime_error("invalid compact float words");};if(s.empty()||expected>4064)bad();const auto align=static_cast<unsigned char>(s[0]);if(align>3||align>expected||s.size()<1+align)bad();std::string out(s.substr(1,align));out.reserve(expected);std::size_t p=1+align;
 while(out.size()+4<=expected){auto n=std::min(std::size_t(32),(expected-out.size())/4);if(s.size()-p<4)bad();std::uint32_t mask;std::memcpy(&mask,s.data()+p,4);p+=4;if(n<32&&(mask>>n))bad();
  for(unsigned i=0;i<n;++i){if(mask&(1U<<i)){if(s.size()-p<2)bad();std::uint16_t h;std::memcpy(&h,s.data()+p,2);p+=2;if((h&0x7c00U)==0x7c00U)bad();float x=minivectordb::detail::half_decode(h);out.append(reinterpret_cast<const char*>(&x),4);}else{if(s.size()-p<4)bad();out.append(s.substr(p,4));p+=4;}}
 }if(s.size()-p!=expected-out.size())bad();out.append(s.substr(p));return out;
}
struct FixedPlan {unsigned width=0;int power=0;};
inline FixedPlan plan(const std::array<std::uint16_t,32>& values,unsigned count) {
 int power=100,high=-100;
 for(unsigned i=0;i<count;++i){
  const auto h=values[i];if(h==0x8000U)return {};
  unsigned m=h&1023,e=(h>>10)&31;if(e)m|=1024;if(!m)continue;
  const int base=e?int(e)-25:-24;
  power=std::min(power,base+int(std::countr_zero(m)));
  // A signed negative power of two needs one fewer magnitude bit. This
  // exponent comparison is exact and avoids a second block conversion pass.
  const int top=base+int(std::bit_width(m))-1;
  high=std::max(high,top-((h&0x8000U)&&std::has_single_bit(m)?1:0));
 }
 if(power==100)return {1,0};
 const unsigned width=unsigned(std::max(1,high-power+2));
 if(width>15 || 2+(width*count+7)/8>=2*count)return {};
 return {width,power};
}
// Compact layout version 1: per-32-word bitmap, optional exact shared binary
// scale, then unchanged raw escapes. No approximation and no row dependencies.
inline bool transform(std::string_view s,std::string& out) {
 const int chosen=choose_alignment(s);if(chosen<0)return false;const unsigned align=unsigned(chosen);std::size_t half_size=1+align+(s.size()-align)%4;
 out.clear();out.reserve(s.size());out+=char(align|4);out.append(s.substr(0,align));std::size_t p=align;
 while(p+4<=s.size()){
  const auto n=std::min(std::size_t(32),(s.size()-p)/4);std::array<std::uint16_t,32> h{};unsigned count=0;std::uint32_t mask=0;
  for(unsigned i=0;i<n;++i){float x=word(s.data()+p+i*4);if(minivectordb::detail::half_exact(x)){mask|=1U<<i;h[count++]=minivectordb::detail::half_encode_exact(x);}}
  out.append(reinterpret_cast<const char*>(&mask),4);half_size+=4+count*2+(n-count)*4;
  if(count){auto fixed=plan(h,count);out+=char(fixed.width);
   if(fixed.width){out+=char(fixed.power);std::uint64_t acc=0;unsigned bits=0;
    for(unsigned i=0;i<count;++i){unsigned mantissa=h[i]&1023,exponent=(h[i]>>10)&31;if(exponent)mantissa|=1024;const int shift=(exponent?int(exponent)-25:-24)-fixed.power;const auto magnitude=mantissa?(shift>=0?mantissa<<shift:mantissa>>(-shift)):0U;const auto integer=(h[i]&0x8000U)?-std::int32_t(magnitude):std::int32_t(magnitude);const auto encoded=std::uint32_t(integer)&((1U<<fixed.width)-1);acc|=std::uint64_t(encoded)<<bits;bits+=fixed.width;
     while(bits>=8){out+=char(acc);acc>>=8;bits-=8;}}
    if(bits)out+=char(acc);
   }else for(unsigned i=0;i<count;++i)out.append(reinterpret_cast<const char*>(&h[i]),2);
  }
  for(unsigned i=0;i<n;++i)if(!(mask&(1U<<i)))out.append(s.substr(p+i*4,4));p+=n*4;
 }
 out.append(s.substr(p));if(out.size()>=half_size)return transform_half(s,out);return out.size()+8<s.size();
}
inline std::string inverse(std::string_view s,std::size_t expected) {
 if(s.empty()||static_cast<unsigned char>(s[0])<4)return inverse_half(s,expected);
 auto bad=[](){throw std::runtime_error("invalid shared-scale compact words");};if(expected>4064)bad();const unsigned tag=static_cast<unsigned char>(s[0]),align=tag&3U;if(tag>7||align>expected||s.size()<1+align)bad();std::string out(s.substr(1,align));out.reserve(expected);std::size_t p=1+align;
 while(out.size()+4<=expected){const auto n=std::min(std::size_t(32),(expected-out.size())/4);if(s.size()-p<4)bad();std::uint32_t mask;std::memcpy(&mask,s.data()+p,4);p+=4;if(n<32&&(mask>>n))bad();const unsigned count=std::popcount(mask);std::array<float,32> values{};
  if(count){if(p==s.size())bad();const unsigned width=static_cast<unsigned char>(s[p++]);if(width>15)bad();
   if(width){if(p==s.size())bad();const int power=std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(s[p++]));if(power< -24||power>15||s.size()-p<(width*count+7)/8)bad();std::uint64_t acc=0;unsigned bits=0;
    for(unsigned i=0;i<count;++i){while(bits<width){acc|=std::uint64_t(static_cast<unsigned char>(s[p++]))<<bits;bits+=8;}std::int32_t integer=acc&((1U<<width)-1);if(integer&(1U<<(width-1)))integer-=1U<<width;acc>>=width;bits-=width;if(integer){const auto magnitude=std::uint32_t(integer<0?-integer:integer);const auto top=31U-std::countl_zero(magnitude);const auto bits32=(integer<0?0x80000000U:0U)|(std::uint32_t(int(top)+power+127)<<23)|((magnitude-(1U<<top))<<(23-top));values[i]=std::bit_cast<float>(bits32);}if(!minivectordb::detail::half_exact(values[i]))bad();}
    if(acc)bad();
   }else {if(s.size()-p<2*count)bad();for(unsigned i=0;i<count;++i){std::uint16_t h;std::memcpy(&h,s.data()+p,2);p+=2;if((h&0x7c00U)==0x7c00U)bad();values[i]=minivectordb::detail::half_decode(h);}}
  }
  unsigned j=0;for(unsigned i=0;i<n;++i){if(mask&(1U<<i)){out.append(reinterpret_cast<const char*>(&values[j++]),4);}else {if(s.size()-p<4)bad();out.append(s.substr(p,4));p+=4;}}
 }
 if(s.size()-p!=expected-out.size())bad();out.append(s.substr(p));return out;
}
}
