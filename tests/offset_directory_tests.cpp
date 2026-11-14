#include "../src/storage/offset_directory.hpp"
#include <map>
#include <random>
#include <iostream>
#include <stdexcept>
using minivectordb::detail::OffsetDirectory;
using Key=OffsetDirectory::Key;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
int main(){try{
  OffsetDirectory d;std::map<Key,std::uint64_t> model;
  std::mt19937_64 rng(20261114);std::size_t checks=0;
  auto verify=[&](){
    for(auto space:{UINT64_C(0),UINT64_C(1),UINT64_C(7),UINT64_MAX}){
      std::vector<std::pair<std::int64_t,std::uint64_t>> expected;
      for(const auto&[k,v]:model)if(k.first==space&&v)expected.push_back({k.second,v});
      CHECK(d.snapshot(space)==expected);++checks;
    }
    for(const auto&[k,v]:model){auto p=d.find(k);CHECK(p&&*p==v);++checks;}
  };
  for(auto space:{UINT64_C(0),UINT64_C(1),UINT64_C(7),UINT64_MAX})
    for(auto k:{INT64_MIN,INT64_MAX}){d.set({space,k},42);model[{space,k}]=42;}
  for(int k=16000;k>=-16000;--k){d.set({7,k},std::uint64_t(k+16001));model[{7,k}]=std::uint64_t(k+16001);}
  verify();
  for(unsigned i=0;i<60000;++i){
    const auto pick=rng()%4;const auto space=pick==3?UINT64_MAX:pick==2?7:pick;
    Key key{space,std::int64_t(rng()%40001)-20000};
    switch(rng()%5){
      case 0:{const bool fresh=model.try_emplace(key,0).second;CHECK(d.ensure(key)==fresh);break;}
      case 1:case 2:{const auto value=rng();d.set(key,value);model[key]=value;break;}
      case 3:d.erase(key);model.erase(key);break;
      default:{const auto p=d.find(key);auto it=model.find(key);CHECK((p!=nullptr)==(it!=model.end()));if(p)CHECK(*p==it->second);break;}
    }
    if(i%997==0){d.purge_zero();for(auto it=model.begin();it!=model.end();)if(!it->second)it=model.erase(it);else ++it;verify();}
  }
  verify();OffsetDirectory other;other.set({1,9},8);d.swap(other);CHECK(d.snapshot(1)==(std::vector<std::pair<std::int64_t,std::uint64_t>>{{9,8}}));d.swap(other);verify();
  for(const auto&[k,v]:model)d.erase(k);CHECK(d.snapshot(7).empty());d.set({0,0},1);CHECK(*d.find({0,0})==1);
  std::cout<<"offset_directory model checks="<<checks<<" passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
