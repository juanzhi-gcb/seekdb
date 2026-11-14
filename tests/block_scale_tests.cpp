#include "../src/vector/int8_bound.hpp"
#include <iostream>
#include <stdexcept>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
using minivectordb::detail::Int8BoundCache;
int main(){try{
  constexpr std::size_t rows=48,dim=128,budget=rows*(dim+16)+(rows/16)*sizeof(double);
  std::vector<float> source(rows*dim,0),q(dim,0);
  std::fill_n(source.begin(),16*dim,1.f);
  std::fill_n(source.begin()+16*dim,16*dim,1000000.f);
  Int8BoundCache cache;CHECK(!cache.build(source,rows,dim,budget-1,true));CHECK(cache.bytes()==0);
  CHECK(cache.build(source,rows,dim,budget,true));CHECK(cache.bytes()==budget);
  auto query=cache.query(q);CHECK(query.valid);
  for(unsigned block=0;block<3;++block){auto values=std::span<const float>(source.data()+block*16*dim,16*dim);
    auto mask=cache.survivors(query,0,values,65535,false);CHECK(mask==(block==2?65535:0));
    if(Int8BoundCache::available()){CHECK(cache.survivors(query,0,values,65535,true)==mask);CHECK(cache.survivors(query,0,values,65535,true,true)==mask);}
    std::cout<<"block="<<block<<" survivors="<<mask<<'\n';
  }
  source.back()=std::numeric_limits<float>::infinity();CHECK(!cache.build(source,rows,dim,budget,true));CHECK(!cache.ready()&&cache.bytes()==0);
  std::cout<<"local scale isolation, exact budget and nonfinite cleanup passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
