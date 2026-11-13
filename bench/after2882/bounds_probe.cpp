#include "vector/certified_candidates.hpp"
#include "vector/validated.hpp"
#include <chrono>
#include <iostream>
#include <random>
#include <string>
using namespace minivectordb;
int main(){try {
  if(!detail::Int8BoundCache::available())throw std::runtime_error("native AVX2 required");
  constexpr std::size_t n=10000,dim=128,nq=200;
  for(const std::string shape:{"continuous","positive","cluster","outlier"}) {
    std::mt19937 gen(20261113);std::uniform_real_distribution<float> uniform(0.f,1.f);
    auto value=[&]{const float u=uniform(gen);return shape=="positive"?u:shape=="cluster"?1.f+u*0.001f:u*32.f-16.f;};
    std::vector<float> data(n*dim),queries(nq*dim),transposed(n*dim);
    for(auto& x:data)x=value();for(auto& x:queries)x=value();
    if(shape=="outlier")for(std::size_t d=0;d<dim;++d)data[(n-1)*dim+d]=1000000.f;
    for(std::size_t row=0;row<n;row+=16)for(std::size_t d=0;d<dim;++d)for(std::size_t lane=0;lane<16;++lane)transposed[row*dim+d*16+lane]=data[(row+lane)*dim+d];
    detail::Int8BoundCache cache;
    if(!cache.build(transposed,n,dim,8*1024*1024))throw std::runtime_error("cache build failed");
    std::vector<std::vector<Neighbor>> truth(nq);
    // Independent full sort, outside timing; never calls candidate bounds.
    for(std::size_t q=0;q<nq;++q){auto& out=truth[q];const auto begin=q%2?(q/2%10)*1000:0,end=q%2?begin+1000:n;
      for(auto row=begin;row<end;++row){double sum=0;for(std::size_t d=0;d<dim;++d){const double delta=double(queries[q*dim+d])-double(data[row*dim+d]);sum+=delta*delta;}out.push_back({std::int64_t(row),std::sqrt(sum)});}
      std::sort(out.begin(),out.end(),[](auto a,auto b){return a.distance<b.distance||(a.distance==b.distance&&a.key<b.key);});out.resize(10);
    }
    for(unsigned round=0;round<3;++round){std::size_t accepted=0,exact=0;std::uint64_t digest=0;
      const auto start=std::chrono::steady_clock::now();
      for(std::size_t q=0;q<nq;++q){const auto begin=q%2?(q/2%10)*1000:0,end=q%2?begin+1000:n;auto query=std::span<const float>(queries.data()+q*dim,dim);
        detail::CandidateStats stats;
        auto result=detail::certified_candidates(cache,query,begin,end,10,[](auto){return true;},[](auto row){return std::int64_t(row);},[&](auto row,auto&){return std::span<const float>(data.data()+row*dim,dim);},stats,true);
        accepted+=bool(result);exact+=stats.exact;
        if(!result){detail::BlockVectorSource blocks=[&](const detail::BlockVectorVisitor& visit){std::array<std::int64_t,16> keys{};for(std::size_t row=begin/16*16;row<end;row+=16){std::uint16_t mask=0;for(unsigned lane=0;lane<16;++lane){keys[lane]=std::int64_t(row+lane);if(row+lane>=begin&&row+lane<end)mask|=1U<<lane;}visit(keys,{transposed.data()+row*dim,16*dim},mask);}};
          result=detail::exact_top_k_transposed_validated_candidates(query,10,blocks,true,true,nullptr,&cache);}
        for(std::size_t k=0;k<10;++k){if((*result)[k].key!=truth[q][k].key||std::bit_cast<std::uint64_t>((*result)[k].distance)!=std::bit_cast<std::uint64_t>(truth[q][k].distance))throw std::runtime_error("independent exact oracle mismatch");digest^=std::uint64_t((*result)[k].key)+std::bit_cast<std::uint64_t>((*result)[k].distance);}
      }
      const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
      std::cout<<"{\"shape\":\""<<shape<<"\",\"round\":"<<round<<",\"ms\":"<<ms<<",\"certified\":"<<accepted<<",\"queries\":"<<nq<<",\"exact_candidates\":"<<exact<<",\"digest\":"<<digest<<",\"oracle_passed\":true}"<<std::endl;
    }
  }
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
