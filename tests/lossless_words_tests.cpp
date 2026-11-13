#include "../src/storage/packed_codec.hpp"
#include <random>
#include <iostream>
#include <stdexcept>
using namespace minivectordb::detail;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
int main(){try{
 std::mt19937 rng(20261113);unsigned cases=0,newframes=0;
 for(unsigned length=0;length<=4064;length+=7)for(unsigned mode=0;mode<5;++mode){std::string original(length,'\0');
  for(char& c:original)c=char(rng());
  if(mode==0)original.assign(length,'a');
  if(mode==1)original.assign(length,'\0');
  if(mode>=2)for(unsigned p=mode%4;p+4<=length;p+=4){float x=mode==4?std::bit_cast<float>(std::uint32_t(rng())):float(int(rng()%4097)-2048)/128;std::memcpy(original.data()+p,&x,4);}
  std::string encoded;
  if(pack_bytes(original,encoded,true,true)) {
    CHECK(unpack_bytes(encoded,4064,true)==original);++cases;
    const bool words=(static_cast<unsigned char>(encoded[3])&64)!=0;newframes+=words;
    if(words){bool rejected=false;try{unpack_bytes(encoded,4064,false);}catch(...){rejected=true;}CHECK(rejected);}
    for(unsigned n=0;n<encoded.size();n+=std::max(1u,unsigned(encoded.size()/7))){bool rejected=false;try{unpack_bytes(std::string_view(encoded).substr(0,n),4064,true);}catch(...){rejected=true;}CHECK(rejected);}
    bool rejected=false;try{unpack_bytes(encoded,original.size()-1,true);}catch(...){rejected=true;}CHECK(rejected);
  }
 }
 // Non-finite bit patterns are raw escapes, never converted; both signs of zero survive.
 std::string raw;for(unsigned i=0;i<100;++i)for(std::uint32_t x:{0U,0x80000000U,0x7f800000U,0xff800000U,0x7fc01234U,0x7f801234U})raw.append(reinterpret_cast<const char*>(&x),4);
 std::string coded;CHECK(pack_bytes(raw,coded,true,true));CHECK(unpack_bytes(coded,4064,true)==raw);
 for(float constant:{0.f,1.f,0.25f}) {
   std::string input(805,'a');for(unsigned p=25;p<537;p+=4)std::memcpy(input.data()+p,&constant,4);
   std::string old,newer;CHECK(pack_bytes(input,old,true));CHECK(pack_bytes(input,newer,true,true));
   CHECK(newer.size()<=old.size());CHECK(unpack_bytes(newer,4064,true)==input);
 }
 CHECK(newframes>100);std::cout<<"lossless words "<<cases<<" cases, "<<newframes<<" new frames; truncation, size bounds, legacy rejection, NaN bits passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
