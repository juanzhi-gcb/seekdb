#pragma once
#include "int8_bound.hpp"
#include <cstring>
namespace minivectordb::detail {
// Lossless auxiliary index: admitted values are integer * 2^exponent.
// n<=4096 and |integer|<=32767 imply squared integer L2 <2^44.
// Thus every reference double subtract/multiply/add is exact, including at
// float32 subnormal magnitudes; the final sqrt is the reference operation.
class Fixed16Cache {
  std::vector<std::int16_t> codes_;
  std::vector<std::int64_t> norms_;
  std::size_t rows_=0,dim_=0;
  int exponent_=0,maximum_=0;
  std::size_t offset(std::size_t r,std::size_t d) const {
    return r/16*16*dim_+d/2*32+r%16*2+d%2;
  }
  static bool integer(float f,int exponent,int& result) {
    if(!std::isfinite(f))return false;
    const double value=std::ldexp(double(f),-exponent);
    if(value < -32767 || value > 32767 || std::trunc(value)!=value)return false;
    result=int(value);return true;
  }
#if defined(__x86_64__) && defined(__GNUC__)
  __attribute__((target("avx2"),noinline))
  static void dot(const std::int16_t* data,const std::uint32_t* q,std::size_t pairs,std::int32_t* out) {
    auto a=_mm256_setzero_si256(),b=a,c=a,d=a;
    for(std::size_t i=0;i<pairs;++i) {
      const auto query=_mm256_set1_epi32(std::bit_cast<std::int32_t>(q[i]));
      const auto lo=_mm256_madd_epi16(query,_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data+i*32)));
      const auto hi=_mm256_madd_epi16(query,_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data+i*32+16)));
      if(i&1){c=_mm256_add_epi32(c,lo);d=_mm256_add_epi32(d,hi);}
      else {a=_mm256_add_epi32(a,lo);b=_mm256_add_epi32(b,hi);}
    }
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out),_mm256_add_epi32(a,c));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out+8),_mm256_add_epi32(b,d));
  }
#endif
public:
  struct Query {std::vector<std::uint32_t> words;std::int64_t norm=0;bool valid=false,simd=false;};
  void clear() noexcept {std::vector<std::int16_t>().swap(codes_);std::vector<std::int64_t>().swap(norms_);rows_=dim_=0;}
  bool ready() const noexcept {return rows_!=0;}
  std::size_t bytes() const noexcept {return codes_.capacity()*2+norms_.capacity()*8;}
  bool build(std::span<const float> aos,std::size_t rows,std::size_t dim,std::size_t budget) {
    clear();if(!rows || dim<32 || dim>4096 || dim%2 || rows>aos.size()/dim)return false;
    const auto padded=(rows+15)/16*16;
    if(padded>budget/(2*dim+8))return false;
    int exponent=128;double largest=0;
    for(float f:aos.first(rows*dim)) {
      const auto bits=std::bit_cast<std::uint32_t>(f),abs=bits&0x7fffffffU;
      if(abs>=0x7f800000U || bits==0x80000000U)return false; // preserve signed-zero payload
      if(!abs)continue;
      const auto e=abs>>23,m=(abs&0x7fffffU)|(e?0x800000U:0);
      exponent=std::min(exponent,(e?int(e)-150:-149)+int(std::countr_zero(m)));
      largest=std::max(largest,std::abs(double(f)));
    }
    if(exponent==128)exponent=0;
    if(std::ldexp(largest,-exponent)>32767)return false;
    try {codes_.assign(padded*dim,0);norms_.assign(padded,0);}catch(const std::bad_alloc&){clear();return false;}
    rows_=rows;dim_=dim;exponent_=exponent;maximum_=int(std::ldexp(largest,-exponent));
    for(std::size_t r=0;r<rows;++r)for(std::size_t d=0;d<dim;++d) {
      int code=0;if(!integer(aos[r*dim+d],exponent,code)){clear();return false;}
      codes_[offset(r,d)]=std::int16_t(code);norms_[r]+=std::int64_t(code)*code;
    }
    return true;
  }
  Query query(std::span<const float> q) const {
    Query out;if(!ready() || q.size()!=dim_ || !Int8BoundCache::environment())return out;
    out.words.assign(dim_/2,0);int maximum=0;
    for(std::size_t d=0;d<dim_;++d) {
      int code;if(!integer(q[d],exponent_,code))return out;
      out.words[d/2]|=std::uint32_t(std::uint16_t(code))<<(16*(d%2));
      out.norm+=std::int64_t(code)*code;maximum=std::max(maximum,std::abs(code));
    }
    // Bound every partial dot sum, not just its final value. Excluding -32768
    // also prevents the single-pair VPMADDWD overflow corner case.
    out.simd=std::int64_t(maximum)*maximum_*dim_<=INT32_MAX;
    out.valid=true;return out;
  }
  void squares(const Query& q,std::size_t block,std::array<std::uint64_t,16>& out,bool native=true) const {
    std::array<std::int32_t,16> dots{};bool fast=false;
#if defined(__x86_64__) && defined(__GNUC__)
    if(native && q.simd && Int8BoundCache::available()){dot(codes_.data()+block*dim_,q.words.data(),dim_/2,dots.data());fast=true;}
#else
    (void)native;
#endif
    for(std::size_t lane=0;lane<16 && block+lane<rows_;++lane) {
      std::int64_t product=dots[lane];
      if(!fast)for(std::size_t d=0;d<dim_;++d) {
        const auto code=std::bit_cast<std::int16_t>(std::uint16_t(q.words[d/2]>>(16*(d%2))));
        product+=std::int64_t(code)*codes_[offset(block+lane,d)];
      }
      out[lane]=std::uint64_t(norms_[block+lane]+q.norm-2*product);
    }
  }
  double distance(std::uint64_t square) const {return std::sqrt(std::ldexp(double(square),2*exponent_));}
};
}
