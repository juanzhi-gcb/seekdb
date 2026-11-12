#include "../src/vector/fixed16.hpp"
#include <random>
#include <iostream>
#include <stdexcept>
using namespace minivectordb;
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);}while(0)
int main(){try{
  std::mt19937 gen(20261112);std::uint64_t checked=0;
  for(std::size_t dim:{32u,128u,1024u,4096u})for(int exponent:{-149,-7,0,80,112}) {
    const std::size_t rows=35;std::vector<float> data(rows*dim),q(dim);
    for(auto& f:data)f=std::ldexp(float(int(gen()%65535)-32767),exponent);
    detail::Fixed16Cache cache;
    CHECK(!cache.build(data,rows,dim,1));CHECK(cache.build(data,rows,dim,1024*1024));
    for(unsigned trial=0;trial<5;++trial){
      for(auto& f:q)f=std::ldexp(float(int(gen()%65535)-32767),exponent);
      if(!trial)std::copy_n(data.data(),dim,q.data());
      auto query=cache.query(q);CHECK(query.valid);
      for(std::size_t block=0;block<rows;block+=16){
        std::array<std::uint64_t,16> a{},b{};cache.squares(query,block,a,false);cache.squares(query,block,b,true);
        for(std::size_t lane=0;lane<16 && block+lane<rows;++lane){
          CHECK(a[lane]==b[lane]);double sum=0;
          for(std::size_t d=0;d<dim;++d){double delta=double(q[d])-double(data[(block+lane)*dim+d]);sum+=delta*delta;}
          CHECK(std::bit_cast<std::uint64_t>(cache.distance(a[lane]))==std::bit_cast<std::uint64_t>(std::sqrt(sum)));++checked;
        }
      }
    }
    auto mode=std::fegetround();std::fesetround(FE_DOWNWARD);CHECK(!cache.query(q).valid);std::fesetround(mode);
  }
  // Exercise the AVX2 non-overflow fast path, including non-full final block.
  std::vector<float> data(35*128),q(128);for(auto& f:data)f=float(int(gen()%4097)-2048)/128;
  detail::Fixed16Cache cache;CHECK(cache.build(data,35,128,100000));
  for(int t=0;t<100;++t){for(auto& f:q)f=float(int(gen()%4097)-2048)/128;auto query=cache.query(q);CHECK(query.valid&&query.simd);
    for(std::size_t block=0;block<35;block+=16){std::array<std::uint64_t,16>a{},b{};cache.squares(query,block,a,false);cache.squares(query,block,b);CHECK(a==b);checked+=16;}}
  q[0]=std::nextafter(q[0],INFINITY);CHECK(!cache.query(q).valid);
  data[0]=-0.f;CHECK(!cache.build(data,35,128,100000));
  data[0]=std::numeric_limits<float>::infinity();CHECK(!cache.build(data,35,128,100000));
  std::cout<<"exact fixed16 comparisons: "<<checked<<"\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
