#include "../src/storage/packed_codec.hpp"
#include "frozen_packed_codec.hpp"
#include <random>
#include <iostream>
#include <chrono>
#include <vector>
#include <stdexcept>
int main(int argc,char**){try{
  std::mt19937 rng(20261114);std::size_t checks=0;
  auto check=[&](const std::string& s){for(bool plane:{false,true}){
    std::string a,b;const bool x=frozen_codec::pack_bytes(s,a,plane),y=minivectordb::detail::pack_bytes(s,b,plane);
    if(x!=y||a!=b)throw std::runtime_error("encoded bytes differ at length "+std::to_string(s.size()));
    if(y&&minivectordb::detail::unpack_bytes(b,4064,plane)!=s)throw std::runtime_error("round trip differs");++checks;
  }};
  for(std::size_t n=0;n<=4100;++n){
    std::string s(n,'\0');check(s);
    for(auto& c:s)c=char(rng());check(s);
    for(std::size_t i=0;i<n;++i)s[i]=char((i/2)%251);check(s);
    for(std::size_t i=0;i<n;++i)s[i]=char((i/3)%251);check(s);
  }
  for(unsigned round=0;round<6000;++round){
    std::string s(64+rng()%4001,'\0');for(auto& c:s)c=char(rng());
    for(unsigned j=0;j<10;++j){auto i=rng()%s.size(),n=std::min<std::size_t>(rng()%150,s.size()-i);std::fill_n(s.begin()+i,n,char(rng()));}check(s);
  }
  std::cout<<"wide codec exact-byte and roundtrip checks="<<checks<<" passed\n";
  if(argc>1){
    for(int shape=0;shape<4;++shape){std::vector<std::string> rows(3000,std::string(768,'\0'));
      for(auto&s:rows)for(std::size_t i=0;i<s.size();++i)s[i]=shape==0?char(rng()):shape==1?char((i/4)%127):shape==2?char(i%4==3?63:rng()):char((i/130)%7);
      for(int round=0;round<3;++round)for(int mode=0;mode<2;++mode){std::string out;std::size_t sum=0;auto start=std::chrono::steady_clock::now();
        for(int repeat=0;repeat<4;++repeat)for(const auto&s:rows){const bool ok=mode?minivectordb::detail::pack_bytes(s,out,true):frozen_codec::pack_bytes(s,out,true);sum+=out.size()+ok;}
        const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        std::cout<<"{\"shape\":"<<shape<<",\"round\":"<<round<<",\"mode\":"<<mode<<",\"ms\":"<<ms<<",\"checksum\":"<<sum<<"}\n";
      }
    }
  }
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
