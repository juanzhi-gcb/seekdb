#include "storage/packed_codec.hpp"
#include <bit>
#include <chrono>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <random>
#include <vector>

using Clock=std::chrono::steady_clock;
using namespace minivectordb;
void require(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
void put(std::string& out,std::uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)out+=char(v>>(8*i));}
std::uint64_t get(std::string_view s,std::size_t pos,unsigned n){require(pos+n<=s.size(),"short field");std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(static_cast<unsigned char>(s[pos+i]))<<(8*i);return v;}
struct Record {std::uint64_t space,key;std::string raw;};
std::vector<Record> load(const char* file){
  std::ifstream in(file,std::ios::binary);require(bool(in),"open corpus");std::string all((std::istreambuf_iterator<char>(in)),{});
  require(all.size()>=8192&&all.substr(0,8)=="MVDBJL08","journal format");std::vector<Record> rows;
  for(std::size_t p=8192;p<all.size();){require(p+48<=all.size()&&all.substr(p,4)=="JREC","record header");
    const auto flag=get(all,p+4,4),length=get(all,p+32,8);require(length<=all.size()-p-48,"record length");auto value=std::string_view(all).substr(p+48,length);
    require(flag<=2,"record flags");if(flag!=2)rows.push_back({get(all,p+8,8),get(all,p+16,8),flag==1?detail::unpack_bytes(value,4064,true):std::string(value)});p+=48+length;
  }return rows;
}
std::vector<Record> general(){std::mt19937 rng(20261112);std::vector<Record> rows;for(int i=0;i<10000;++i){std::string r;put(r,1,1);put(r,4,4);put(r,i,8);put(r,i%10,8);put(r,128,4);for(int d=0;d<128;++d){float f=std::uniform_real_distribution<float>(-16,16)(rng);put(r,std::bit_cast<std::uint32_t>(f),4);}std::string body;for(int j=0;j<256;++j)body+=char(32+rng()%95);put(r,body.size(),4);r+=body;rows.push_back({2,std::uint64_t(i),std::move(r)});}return rows;}
