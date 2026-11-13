#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>
namespace minivectordb::detail {
// An exact power-of-two lattice proved over the complete immutable input.
// Negative zero and heterogeneous precision decline admission, never round.
struct SharedCodePlan {
  int fine=128; double maximum=0; bool valid=true;
  void observe(float x) noexcept {
    const auto bits=std::bit_cast<std::uint32_t>(x),mag=bits&0x7fffffffU;
    if(bits==0x80000000U || mag>=0x7f800000U){valid=false;return;}
    if(!mag)return;
    const auto e=mag>>23,m=(mag&0x7fffffU)|(e?0x800000U:0);
    fine=std::min(fine,(e?int(e)-150:-149)+int(std::countr_zero(m)));
    maximum=std::max(maximum,std::abs(double(x)));
  }
  int shift() const noexcept {
    if(!valid)return -1;
    if(!maximum)return 0;
    // Both operands are exact powers of two or binary32, independent of FENV.
    int coarse=fine;
    while(std::ldexp(127.0,coarse)<maximum)++coarse;
    return coarse-fine;
  }
  unsigned width() const noexcept {const int s=shift();return s<0||s>6?99U:(s?unsigned(s+1):0U);}
  int power() const noexcept {return maximum?fine:0;}
};
class SharedCodes {
  std::unique_ptr<std::int8_t[]> codes_;
  std::unique_ptr<unsigned char[]> residuals_;
  std::size_t rows_=0,dim_=0,padded_=0,stride_=0;
  int fine_=0,shift_=0;unsigned width_=0;bool transposed_=false;
  std::size_t code_offset(std::size_t row,std::size_t d) const noexcept {
    return transposed_&&row<rows_/16*16?(row/16*16)*padded_+d/4*64+row%16*4+d%4:row*padded_+d;
  }
public:
  void clear() noexcept {codes_.reset();residuals_.reset();rows_=dim_=padded_=stride_=0;transposed_=false;}
  bool ready() const noexcept {return bool(codes_);}
  static std::size_t row_bytes(std::size_t d,unsigned w) noexcept {return (d+3)/4*4+(d*w+7)/8;}
  std::size_t row_bytes() const noexcept {return padded_+stride_;}
  std::size_t bytes() const noexcept {return rows_*row_bytes();}
  const std::int8_t* codes() const noexcept {return codes_.get();}
  double scale() const noexcept {return std::ldexp(1.0,fine_+shift_);}
  void allocate(std::size_t rows,std::size_t dim,const SharedCodePlan& plan) {
    clear();rows_=rows;dim_=dim;padded_=(dim+3)/4*4;width_=plan.width();
    fine_=plan.power();shift_=plan.shift();stride_=(dim*width_+7)/8;
    codes_=std::make_unique<std::int8_t[]>(rows*padded_);
    residuals_=std::make_unique<unsigned char[]>(rows*stride_);
  }
  bool append(std::size_t row,std::span<const float> values) noexcept {
    for(std::size_t d=0;d<dim_;++d){
      const auto x=values[d];if(!std::isfinite(x)||std::bit_cast<std::uint32_t>(x)==0x80000000U)return false;
      const double scaled=std::ldexp(double(x),-fine_);
      if(std::abs(scaled)>16384 || std::trunc(scaled)!=scaled)return false;
      const int n=int(scaled),unit=1<<shift_;
      const int code=n<0?-((-n+unit/2)/unit):(n+unit/2)/unit;
      if(code < -127 || code > 127)return false;
      const int residual=n-code*unit;
      if(width_ && (residual<-(1<<shift_) || residual>=(1<<shift_)))return false;
      codes_[row*padded_+d]=static_cast<std::int8_t>(code);
      const unsigned encoded=unsigned(residual)&((1U<<width_)-1);
      const auto bit=d*width_;auto* p=residuals_.get()+row*stride_;
      if(width_){p[bit/8]|=static_cast<unsigned char>(encoded<<(bit%8));
        if(bit%8+width_>8)p[bit/8+1]|=static_cast<unsigned char>(encoded>>(8-bit%8));}
    }
    return true;
  }
  float value(std::size_t row,std::size_t d) const noexcept {
    int residual=0;
    if(width_){const auto bit=d*width_;const auto* p=residuals_.get()+row*stride_;
      unsigned v=p[bit/8];if(bit%8+width_>8)v|=unsigned(p[bit/8+1])<<8;
      v=(v>>(bit%8))&((1U<<width_)-1);
      residual=(v&(1U<<(width_-1)))?int(v)-int(1U<<width_):int(v);
    }
    return std::ldexp(float(int(codes_[code_offset(row,d)])*(1<<shift_)+residual),fine_);
  }
  void save(std::size_t row,void* output) const noexcept {
    auto* p=static_cast<unsigned char*>(output);std::memcpy(p,codes_.get()+row*padded_,padded_);
    if(stride_)std::memcpy(p+padded_,residuals_.get()+row*stride_,stride_);
  }
  void load(std::size_t row,const void* input) noexcept {
    auto* p=static_cast<const unsigned char*>(input);std::memcpy(codes_.get()+row*padded_,p,padded_);
    if(stride_)std::memcpy(residuals_.get()+row*stride_,p+padded_,stride_);
  }
  void move(std::size_t to,std::size_t from) noexcept {
    std::memcpy(codes_.get()+to*padded_,codes_.get()+from*padded_,padded_);
    if(stride_)std::memcpy(residuals_.get()+to*stride_,residuals_.get()+from*stride_,stride_);
  }
  void transpose() {
    std::vector<std::int8_t> scratch(16*padded_);
    for(std::size_t r=0;r+16<=rows_;r+=16){
      auto* p=codes_.get()+r*padded_;
      for(std::size_t lane=0;lane<16;++lane)for(std::size_t d=0;d<padded_;++d)
        scratch[d/4*64+lane*4+d%4]=p[lane*padded_+d];
      std::memcpy(p,scratch.data(),scratch.size());
    }
    transposed_=true;
  }
};
}
