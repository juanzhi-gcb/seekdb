#include "minivectordb/exact.hpp"
#include "../src/vector/validated.hpp"
#include "../src/vector/int8_bound.hpp"
#include <array>
#include <bit>
#include <random>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <algorithm>
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);}while(false)
using namespace minivectordb;
int main(){try{
 std::mt19937 rng(20261114);std::size_t checks=0;
 for(const std::size_t dim:{17,32,128,129,4096})for(int shape=0;shape<4;++shape){
  constexpr std::size_t blocks=19,rows=blocks*16;
  std::vector<float> data(rows*dim),transposed(rows*dim),query(dim);
  std::vector<std::int64_t> keys(rows);std::array<std::uint16_t,blocks> masks{};
  for(auto&q:query)q=shape==3?0:float(int(rng()%200)-100)/32;
  for(std::size_t i=0;i<rows;++i){keys[i]=std::int64_t(rows-i)-152;
   for(std::size_t j=0;j<dim;++j){float v=shape==3?(i%16==0?0.f:1000.f):shape==0?float(int(rng()%200)-100)/32:shape==1?query[j]:((i+j)%3==0?std::numeric_limits<float>::max():std::numeric_limits<float>::denorm_min());data[i*dim+j]=v;transposed[(i/16)*dim*16+j*16+i%16]=v;}}
  detail::Int8BoundCache cache;cache.build(transposed,rows,dim,rows*(dim+16)+blocks*sizeof(double),true);
  for(int pattern=0;pattern<5;++pattern){
   for(std::size_t b=0;b<blocks;++b){std::uint16_t m=0;unsigned n=pattern==4?16:pattern==0?1:pattern==1?3:pattern==2?unsigned(b%5):unsigned(b%3==0?16:b%3);for(unsigned j=0;j<n;++j)m|=std::uint16_t(1U<<((j*5+b)%16));masks[b]=m;}
   VectorSource scalar=[&](const VectorVisitor& visit){for(std::size_t i=0;i<rows;++i)if(masks[i/16]&(1U<<(i%16)))visit(keys[i],std::span<const float>(data.data()+i*dim,dim));};
   // Deliberately reuse a callback buffer: the optimized path must own sparse
   // lanes before the next block overwrites this memory.
   detail::BlockVectorSource source=[&](const detail::BlockVectorVisitor& visit){std::vector<float> scratch(dim*16);for(std::size_t b=0;b<blocks;++b){std::copy_n(transposed.data()+b*dim*16,dim*16,scratch.data());visit(std::span<const std::int64_t>(keys.data()+b*16,16),scratch,masks[b]);}};
   for(std::size_t k:{0,1,10,27,400}){auto want=exact_top_k(query,k,scalar);
    for(bool wide:{false,true})for(bool screen:{false,true}){auto got=detail::exact_top_k_transposed_validated_candidates(query,k,source,wide,screen,nullptr,nullptr);CHECK(got.size()==want.size());for(std::size_t i=0;i<want.size();++i){CHECK(got[i].key==want[i].key);CHECK(std::bit_cast<std::uint64_t>(got[i].distance)==std::bit_cast<std::uint64_t>(want[i].distance));}++checks;}
    detail::BlockVectorSource stable=[&](const detail::BlockVectorVisitor& visit){for(std::size_t b=0;b<blocks;++b)visit(std::span<const std::int64_t>(keys.data()+b*16,16),std::span<const float>(transposed.data()+b*dim*16,dim*16),masks[b]);};
    auto got=detail::exact_top_k_transposed_validated_candidates(query,k,stable,true,true,nullptr,&cache);CHECK(got.size()==want.size());for(std::size_t i=0;i<want.size();++i){CHECK(got[i].key==want[i].key);CHECK(std::bit_cast<std::uint64_t>(got[i].distance)==std::bit_cast<std::uint64_t>(want[i].distance));}++checks;
   }
  }
 }
 std::cout<<"exact sparse/dense masks, reusable source, sqrt ties, extremes and K: "<<checks<<" checks; avx2="<<detail::has_avx2()<<" avx512="<<detail::has_avx512()<<'\n';
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
