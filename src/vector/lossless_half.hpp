#pragma once
#include <bit>
#include <cstdint>
namespace minivectordb::detail {
// Bit-only codec. Admission requires exact representability; never rounds.
inline bool half_exact(float x) noexcept {
 const auto b=std::bit_cast<std::uint32_t>(x)&0x7fffffffU;
 if(!b)return true;
 const auto e=b>>23,m=(b&0x7fffffU)|0x800000U;
 if(e>=113&&e<=142)return (m&8191U)==0;
 if(e>=103&&e<=112)return (m&((1U<<(126-e))-1))==0;
 return false;
}
inline std::uint16_t half_encode_exact(float x) noexcept {
 const auto b=std::bit_cast<std::uint32_t>(x),e=(b>>23)&255,m=b&0x7fffffU;
 const auto sign=(b>>16)&0x8000U;
 if(!(b&0x7fffffffU))return static_cast<std::uint16_t>(sign);
 if(e>=113)return static_cast<std::uint16_t>(sign|((e-112)<<10)|(m>>13));
 return static_cast<std::uint16_t>(sign|((m|0x800000U)>>(126-e)));
}
inline float half_decode(std::uint16_t x) noexcept {
 const std::uint32_t sign=std::uint32_t(x&0x8000U)<<16,e=(x>>10)&31,m=x&1023;
 if(e)return std::bit_cast<float>(sign|((e+112)<<23)|(m<<13));
 if(!m)return std::bit_cast<float>(sign);
 const auto top=31U-std::countl_zero(m);
 return std::bit_cast<float>(sign|((top+103)<<23)|((m-(1U<<top))<<(23-top)));
}
}
