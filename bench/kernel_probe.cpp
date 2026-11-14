#include "minivectordb/exact.hpp"
#include "validated.hpp"
#include "int8_bound.hpp"
#include <random>
#include <iostream>
#include <bit>
#include <chrono>
int main(){
 for(size_t n:{10000u,50000u})for(unsigned shape=0;shape<3;++shape){constexpr size_t d=128;std::mt19937 rng(20261115);
 std::vector<float> v(n*d);for(auto& x:v)x=shape==2?1.f+float(rng()%1000)*0.000001f:float(int(rng()%2000001)-1000000)/65536.f;
 if(shape==1)for(size_t j=0;j<d;++j)v[(n-16)*d+j*16+15]=1000000.f;
 std::vector<int64_t> scalar(n*2);for(size_t r=0;r<n;++r){scalar[2*r]=int64_t(n-r)-100;scalar[2*r+1]=int64_t(r%10);}
 minivectordb::detail::Int8BoundCache cache;if(!cache.build(v,n,d,16*1024*1024,true))return 2;
 std::vector<std::vector<float>> queries(16,std::vector<float>(d));for(auto&q:queries)for(auto&x:q)x=shape==2?1.f+float(rng()%1000)*0.000001f:float(int(rng()%2000001)-1000000)/65536.f;
 for(unsigned round=0;round<3;++round){uint64_t checked=0,rejected=0,exact=0,hash=0;const auto start=std::chrono::steady_clock::now();
 for(auto&query:queries){minivectordb::detail::BoundScreenStats stats;size_t base=0;
 auto result=minivectordb::detail::exact_top_k_transposed_validated_candidates(query,10,[&](const auto&visit){
   std::array<int64_t,16> keys{};
   for(size_t r=0;r<n;r+=16){base=r;
#ifndef LAZY_KEYS
     for(size_t k=0;k<16;++k)keys[k]=scalar[2*(r+k)];
#endif
     visit(std::span<const int64_t>(keys),std::span<const float>(v.data()+r*d,16*d),uint16_t(65535));}
 },true,true,&stats,&cache
#ifdef LAZY_KEYS
 ,[&](size_t lane){return scalar[2*(base+lane)];}
#endif
 );
 checked+=stats.integer_screened;rejected+=stats.integer_rejected;exact+=stats.exact_lanes;
 for(auto&x:result)hash=(hash*1099511628211ULL)^std::bit_cast<uint64_t>(x.distance)^uint64_t(x.key);
 }
 const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 std::cout<<"{\"rows\":"<<n<<",\"shape\":"<<shape<<",\"round\":"<<round<<",\"ms\":"<<ms<<",\"screened\":"<<checked<<",\"rejected\":"<<rejected<<",\"exact\":"<<exact<<",\"result_hash\":"<<hash<<"}\n";
 }
 }
}
