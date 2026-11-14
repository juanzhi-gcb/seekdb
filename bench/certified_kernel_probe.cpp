#include "vector/certified_candidates.hpp"
#include "vector/validated.hpp"
#include <bit>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace minivectordb;
bool half_exact(float x){auto bits=std::bit_cast<std::uint32_t>(x)&0x7fffffffU;if(!bits)return true;const auto e=bits>>23,m=(bits&0x7fffffU)|0x800000U;if(e>=113&&e<=142)return (m&8191U)==0;if(e>=103&&e<=112)return (m&((1U<<(126-e))-1))==0;return false;}
int main(){try {
  if(!detail::Int8BoundCache::available()){std::cout<<"{\"unsupported\":true}\n";return 0;}
  constexpr std::size_t n=10000,dim=128,nq=200;
  for(const auto distribution:{"grid","continuous","equal"}){
    std::mt19937 gen(20261113);std::vector<float> vectors(n*dim),queries(nq*dim);std::size_t half=0;
    auto component=[&]{if(std::string(distribution)=="grid")return float(int(gen()%4097)-2048)/128;
      if(std::string(distribution)=="equal")return 1.f;
      return float(double(gen())/double(UINT32_MAX)*32-16);};
    for(auto& x:vectors){x=component();half+=half_exact(x);}for(auto& x:queries)x=component();
    std::vector<float> transposed(n*dim);
    for(std::size_t b=0;b<n;b+=16)for(std::size_t d=0;d<dim;++d)for(std::size_t l=0;l<16;++l)transposed[b*dim+d*16+l]=vectors[(b+l)*dim+d];
    detail::Int8BoundCache cache;if(!cache.build(transposed,n,dim,8*1024*1024))throw std::runtime_error("cache build failed");
    std::vector<std::vector<Neighbor>> reference(nq);
    for(int round=0;round<3;++round)for(int phase=0;phase<2;++phase){const int mode=round%2?1-phase:phase;
      std::size_t accepted=0,attempted_exact=0;std::uint64_t digest=0;
      const auto at=std::chrono::steady_clock::now();
      for(std::size_t q=0;q<nq;++q){const auto query=std::span<const float>(queries.data()+q*dim,dim);const auto begin=q%2?(q/2%10)*1000:0,end=q%2?begin+1000:n;
        const detail::BlockVectorSource source=[&](const detail::BlockVectorVisitor& visit){std::array<std::int64_t,16> keys{};
          for(std::size_t b=begin/16*16;b<end;b+=16){std::uint16_t mask=0;for(std::size_t l=0;l<16;++l){keys[l]=std::int64_t(n-b-l);if(b+l>=begin&&b+l<end)mask|=std::uint16_t(1U<<l);}visit(keys,{transposed.data()+b*dim,dim*16},mask);}};
        std::optional<std::vector<Neighbor>> result;
        if(mode){detail::CandidateStats stats;result=detail::certified_candidates(cache,query,begin,end,10,[](auto){return true;},[&](auto r){return std::int64_t(n-r);},[&](auto r,auto&){return std::span<const float>(vectors.data()+r*dim,dim);},stats,true);accepted+=bool(result);attempted_exact+=stats.exact;}
        if(!result)result=detail::exact_top_k_transposed_validated_candidates(query,10,source,true,true,nullptr,&cache);
        if(round==0&&mode==0)reference[q]=*result;
        else {if(result->size()!=reference[q].size())throw std::runtime_error("result size");for(std::size_t i=0;i<result->size();++i)if((*result)[i].key!=reference[q][i].key||std::bit_cast<std::uint64_t>((*result)[i].distance)!=std::bit_cast<std::uint64_t>(reference[q][i].distance))throw std::runtime_error("exact result mismatch");}
        for(auto x:*result)digest^=std::bit_cast<std::uint64_t>(x.distance)+std::uint64_t(x.key);
      }
      const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-at).count();
      std::cout<<"{\"distribution\":\""<<distribution<<"\",\"round\":"<<round+1<<",\"candidate\":"<<mode<<",\"ms\":"<<ms<<",\"certified\":"<<accepted<<",\"queries\":"<<nq<<",\"attempted_exact\":"<<attempted_exact<<",\"half_exact_components\":"<<half<<",\"components\":"<<vectors.size()<<",\"digest\":"<<digest<<",\"exact\":true}"<<std::endl;
    }
  }
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
