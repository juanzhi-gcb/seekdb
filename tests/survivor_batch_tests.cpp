#define main original_validated_test_main
#include "validated_vector_tests.cpp"
#undef main
#include <array>
int main(){try{
  std::mt19937 rng(20261114);std::size_t checks=0;
  for(std::size_t dim:{1u,31u,32u,128u,129u})for(unsigned shape=0;shape<4;++shape){
    std::vector<Item> data;std::vector<float> q(dim,0.25f);
    for(int i=0;i<277;++i){std::vector<float> v(dim);
      for(auto&x:v)x=shape==0?float(int(rng()%4097)-2048)/128.f:shape==1?1.f:shape==2?arbitrary_finite(rng):std::bit_cast<float>(std::uint32_t(rng())&0x807fffffu);
      data.push_back({276-i,std::move(v)});
    }
    for(unsigned retained:{1u,2u,3u,4u,7u,8u,15u,16u})for(std::size_t k:{1u,10u,300u})for(bool wide:{false,true}){
      std::vector<Item> selected;
      auto source=[&](const detail::BlockVectorVisitor& visit){
        std::vector<float> block(16*dim,0);std::array<std::int64_t,16> keys{};
        for(std::size_t r=0;r<data.size();r+=16){auto n=std::min(std::size_t(16),data.size()-r);std::uint16_t mask=0;
          for(std::size_t lane=0;lane<n;++lane){keys[lane]=data[r+lane].key;for(std::size_t d=0;d<dim;++d)block[d*16+lane]=data[r+lane].value[d];if((lane+r/16)%16<retained)mask|=std::uint16_t(1u<<lane);}
          visit({keys.data(),n},block,mask);
          // The source may immediately reuse or overwrite its callback storage.
          std::fill(block.begin(),block.end(),-777.f);
        }
      };
      for(std::size_t r=0;r<data.size();++r)if((r%16+(r/16))%16<retained)selected.push_back(data[r]);
      same(detail::exact_top_k_transposed_validated_candidates(q,k,source,wide,true,nullptr),oracle(selected,q,k,{}));++checks;
    }
  }
  std::cout<<"survivor batches exact-oracle checks="<<checks<<" avx2="<<detail::has_avx2()<<" passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
