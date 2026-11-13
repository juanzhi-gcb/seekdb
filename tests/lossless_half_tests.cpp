#include "../src/sql/typed_scan_cache.hpp"
#include <iostream>
#include <random>
#include <stdexcept>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
int main(){try{
 for(unsigned bits=0;bits<65536;++bits){
   if(((bits>>10)&31)==31)continue;
   // Independent IEEE binary16 numeric definition, including negative zero.
   const unsigned exponent=(bits>>10)&31,mantissa=bits&1023;
   float expected=exponent?std::ldexp(float(1024+mantissa),int(exponent)-25):std::ldexp(float(mantissa),-24);
   if(bits&32768)expected=-expected;
   const float actual=detail::half_decode(static_cast<std::uint16_t>(bits));
   CHECK(std::bit_cast<std::uint32_t>(actual)==std::bit_cast<std::uint32_t>(expected));
   CHECK(detail::half_exact(expected));CHECK(detail::half_encode_exact(expected)==bits);
 }
 std::mt19937 random(20261113);
 for(unsigned i=0;i<1000000;++i){const float x=std::bit_cast<float>(static_cast<std::uint32_t>(random()));
   if(detail::half_exact(x))CHECK(std::bit_cast<std::uint32_t>(detail::half_decode(detail::half_encode_exact(x)))==std::bit_cast<std::uint32_t>(x));
 }
 Schema schema{91,"t",{{"id",ColumnType::Bigint,0},{"v",ColumnType::Vector,128},{"g",ColumnType::Bigint,0}},0};
 sql::TypedScanCache cache;
 for(unsigned cycle=0;cycle<4;++cycle){
   const bool eligible=cycle%2==0;cache.invalidate();
   std::vector<std::vector<float>> vectors(531,std::vector<float>(128));
   for(unsigned row=0;row<vectors.size();++row)for(unsigned d=0;d<128;++d)vectors[row][d]=d%3?float(int(random()%4097)-2048)/128:-0.f;
   if(!eligible)vectors.back().back()=std::nextafter(1.f,2.f);
   cache.start(schema,0,1,2*1024*1024,true,{},true);
   for(const auto& v:vectors)cache.observe_row({},v);cache.finish();
   // Shrinking shared budget must preserve the admission proof.
   cache.start(schema,0,1,1024*1024,true,{},true);
   CHECK(cache.building());CHECK(cache.lossless_half()==eligible);
   for(std::size_t r=0;r<vectors.size();++r)cache.append(vectors[r],[&](auto c){return c==0?std::int64_t(r):std::int64_t(r%7);});
   cache.finish(true,true,true,true);CHECK(cache.ready(91,0,1));
   std::vector<float> scratch;
   for(std::size_t r=0;r<cache.size();++r){auto v=cache.read_vector(r,scratch);const auto& expected=vectors.at(cache.key(r));
     for(unsigned d=0;d<128;++d)CHECK(std::bit_cast<std::uint32_t>(v[d])==std::bit_cast<std::uint32_t>(expected[d]));
   }
   if(eligible)CHECK(cache.bytes()<531*(128*4+16));
 }
 // Unknown/incomplete probation never enables the compact representation.
 cache.invalidate();cache.start(schema,0,1,1024*1024,true,{},true);cache.start(schema,0,1,1024*1024,true,{},true);CHECK(!cache.lossless_half());
 std::cout<<"lossless half: 63488 finite encodings, 1000000 float patterns, cache admission/cluster/invalidation/fallback passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
