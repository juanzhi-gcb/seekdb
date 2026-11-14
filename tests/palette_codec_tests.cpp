#include "../src/storage/palette_codec.hpp"
#include <random>
#include <iostream>
using namespace minivectordb::detail;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
int main(){try{
  std::mt19937 rng(20261114);std::size_t checks=0,palettes=0;
  auto reject=[&](std::string_view s){bool caught=false;try{(void)unpack_journal_bytes(s);}catch(const std::runtime_error&){caught=true;}CHECK(caught);++checks;};
  auto check=[&](const std::string& s,bool malformed){
    std::string out,old;const bool ok=pack_journal_bytes(s,out),prior=pack_bytes(s,old,true);
    if(prior){CHECK(ok&&out.size()<=old.size());CHECK(unpack_journal_bytes(old)==s);}
    if(ok){CHECK(out.size()+8<=s.size());CHECK(unpack_journal_bytes(out)==s);++checks;
      if(static_cast<unsigned char>(out[3])&64U)++palettes;
      if(malformed){for(std::size_t n=0;n<out.size();++n)reject(std::string_view(out).substr(0,n));reject(out+'x');}
    }
  };
  for(std::size_t n=0;n<=4100;++n){std::string s(n,'x');check(s,false);
    for(auto&c:s)c=char(rng());check(s,false);
    for(unsigned k=1;k<=5;++k){for(auto&c:s)c=char(rng()%(1U<<k));check(s,false);}
  }
  for(unsigned i=0;i<2000;++i){std::string s(64+rng()%4001,'\0');
    for(std::size_t j=0;j<s.size();++j)s[j]=char((j%4==3)?rng()%8:rng());check(s,i<20);
    auto p=palette_frame(s);if(p.size()+8<=s.size()){
      CHECK(unpack_journal_bytes(p)==s);++checks;
      for(unsigned j=0;j<8;++j){auto corrupt=p;corrupt[rng()%corrupt.size()]^=char(1U<<(rng()%8));try{auto decoded=unpack_journal_bytes(corrupt);CHECK(decoded.size()<=4064);}catch(const std::runtime_error&){}++checks;}
    }
  }
  CHECK(palettes>1000);
  for(unsigned i=0;i<20000;++i){std::string random(4+rng()%1024,'\0');for(auto&c:random)c=char(rng());random[3]|=64;try{auto decoded=unpack_journal_bytes(random);CHECK(decoded.size()<=4064);}catch(const std::runtime_error&){}++checks;}
  std::cout<<"palette checks="<<checks<<" selected="<<palettes<<" passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
