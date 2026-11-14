#include "minivectordb/exact.hpp"
#include "../src/vector/validated.hpp"
#include "../src/vector/int8_bound.hpp"
#include <random>
#include <bit>
#include <iostream>
#include <stdexcept>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
using namespace minivectordb;
int main(){try{
 std::mt19937 rng(20261115);unsigned cases=0;
 for(unsigned d:{32u,127u,128u,129u})for(unsigned shape=0;shape<5;++shape){
  const size_t n=shape==0?1053:139,full=n/16*16;
  std::vector<int64_t> keys(n);std::vector<std::vector<float>> rows(n,std::vector<float>(d));
  for(size_t i=0;i<n;++i){keys[i]=int64_t((n-i)*3)-200;for(auto&x:rows[i])x=shape==1?1.f:shape==2?float(int(rng()%3)-1):float(int(rng()%131071)-65535)/4096.f;}
  if(shape==3)for(auto&x:rows.back())x=1000000.f;
  if(shape==4)for(size_t i=0;i<n;++i)for(auto&x:rows[i])x=std::ldexp(x,int(i%9)-4);
  std::vector<float> store(full*d);
  for(size_t r=0;r<full;r+=16)for(size_t j=0;j<d;++j)for(size_t lane=0;lane<16;++lane)store[r*d+j*16+lane]=rows[r+lane][j];
  detail::Int8BoundCache cache;CHECK(cache.build(store,n,d,16*1024*1024,true));
  for(unsigned trial=0;trial<5;++trial)for(size_t k:{size_t(0),size_t(1),size_t(10),n+2})for(bool bound:{false,true}){
   std::vector<float> q=rows[trial];for(auto&x:q)if(trial%2)x=float(int(rng()%131071)-65535)/4096.f;
   auto selected=[&](int64_t key){return trial==0 || (trial==1?false:key%int64_t(trial+1)!=0);};
   const auto expected=exact_top_k(q,k,[&](const VectorVisitor&visit){for(size_t i=0;i<n;++i)visit(keys[i],rows[i]);},selected);
   size_t base=0,lookups=0;detail::BoundScreenStats stats;
   auto result=detail::exact_top_k_transposed_validated_candidates(q,k,[&](const detail::BlockVectorVisitor&visit){
    std::array<int64_t,16> dummy{};std::vector<float> tail(16*d);
    for(size_t r=0;r<n;r+=16){const auto count=std::min(size_t(16),n-r);uint16_t mask=0;
     for(size_t lane=0;lane<count;++lane)if(selected(keys[r+lane]))mask|=uint16_t(1u<<lane);
     base=r;
     if(count==16)visit({dummy.data(),count},{store.data()+r*d,16*d},mask);
     else{std::fill(tail.begin(),tail.end(),0);for(size_t j=0;j<d;++j)for(size_t lane=0;lane<count;++lane)tail[j*16+lane]=rows[r+lane][j];visit({dummy.data(),count},tail,mask);}
    }
   },true,bound,&stats,&cache,[&](size_t lane){CHECK(base+lane<n);++lookups;return keys[base+lane];});
   CHECK(result.size()==expected.size());for(size_t i=0;i<result.size();++i){CHECK(result[i].key==expected[i].key);CHECK(std::bit_cast<uint64_t>(result[i].distance)==std::bit_cast<uint64_t>(expected[i].distance));}
   CHECK(lookups==stats.exact_lanes);++cases;
  }
 }
 std::cout<<cases<<" lazy-key scalar-oracle cases, ties, masks, changing blocks and tails passed\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
