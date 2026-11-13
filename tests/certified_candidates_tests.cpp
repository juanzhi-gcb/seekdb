#define main original_validated_main
#include "validated_vector_tests.cpp"
#undef main
#include "../src/vector/certified_candidates.hpp"
#include <cfenv>
int main(){try {
  std::size_t accepted=0,fallbacks=0,checked=0;
  for(auto dimension:{32u,127u,128u,129u,1024u})for(unsigned mode=0;mode<5;++mode){
    std::mt19937 gen(20261113+dimension+mode);const std::size_t rows=531;
    std::vector<Item> data;
    for(std::size_t r=0;r<rows;++r){std::vector<float> v(dimension);
      for(auto& x:v){if(mode==0)x=float(int(gen()%4097)-2048)/128;
        else if(mode==1)x=arbitrary_finite(gen);
        else if(mode==2)x=gen()%2?1.f:std::nextafter(1.f,2.f);
        else if(mode==3)x=std::bit_cast<float>(std::uint32_t(gen())&0x807fffffu);
        else x=gen()%2?0.f:-0.f;}
      data.push_back({std::int64_t(rows-r)-270,std::move(v)});
    }
    std::vector<float> transposed(rows/16*16*dimension);
    for(std::size_t r=0;r<rows/16*16;r+=16)for(std::size_t d=0;d<dimension;++d)for(std::size_t l=0;l<16;++l)transposed[r*dimension+d*16+l]=data[r+l].value[d];
    detail::Int8BoundCache cache;CHECK(cache.build(transposed,rows,dimension,8*1024*1024,true));
    for(unsigned trial=0;trial<4;++trial){auto q=data[trial*11].value;
      if(trial==1)for(auto& x:q)x=float(int(gen()%4097)-2048)/128;
      for(std::size_t k:{1u,10u,16u}){
        const auto predicate=[&](std::size_t r){return trial<2||data[r].key%7==0;};
        detail::CandidateStats stats;
        auto actual=detail::certified_candidates(cache,q,0,rows,k,predicate,[&](auto r){return data[r].key;},[&](auto r,auto&){return std::span<const float>(data[r].value);},stats);
        if(actual){same(*actual,oracle(data,q,k,[&](auto key){return trial<2||key%7==0;}));++accepted;}
        else ++fallbacks;
        CHECK(stats.exact<=64);++checked;
      }
    }
    auto q=data[0].value;detail::CandidateStats stats;
    const auto rounding=std::fegetround();CHECK(std::fesetround(FE_UPWARD)==0);
    CHECK(!detail::certified_candidates(cache,q,0,rows,10,[](auto){return true;},[&](auto r){return data[r].key;},[&](auto r,auto&){return std::span<const float>(data[r].value);},stats));
    CHECK(std::fesetround(rounding)==0);
  }
  CHECK(accepted>10&&fallbacks>10);
  std::cout<<"{\"checked\":"<<checked<<",\"accepted\":"<<accepted<<",\"fallbacks\":"<<fallbacks<<"}\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
