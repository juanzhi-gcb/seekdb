#include "minivectordb/store.hpp"
#include "minivectordb/profile.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <sys/resource.h>
using namespace minivectordb;
using Clock=std::chrono::steady_clock;
int main(int argc,char**argv){try{
  if(argc!=4)throw std::runtime_error("probe fresh-db cache-bytes corpus");
  const std::filesystem::path path=argv[1];if(std::filesystem::exists(path))throw std::runtime_error("refuse existing probe database");
  const auto budget=std::stoull(argv[2]);const bool general=std::string(argv[3])=="general";
  std::mt19937 rng(20261113);constexpr int n=10000;
  std::vector<std::string> values(n);
  for(int i=0;i<n;++i){auto&v=values[i];v.resize(8+128*4);std::memcpy(v.data(),&i,sizeof(i));
    for(int j=0;j<128;++j){float f=general?float(double(rng())/double(UINT32_MAX)*32-16):float(int(rng()%4097)-2048)/128;std::memcpy(v.data()+8+j*4,&f,4);}
    v+="record-"+std::to_string(i)+"-";for(int j=0;j<256;++j)v+=general?char(32+rng()%95):char('a'+i%26);
  }
  std::vector<int> order(n);std::iota(order.begin(),order.end(),0);std::shuffle(order.begin(),order.end(),rng);
  auto open=[&]{return std::make_unique<Store>(path,0,true,0,budget,CommitSyncPolicy::ProcessCrash,true,true,true,false,true,true,true
#ifdef SEGMENT_CANDIDATE
  ,true
#endif
  );};
  auto db=open();const auto start=Clock::now();
  for(int i=0;i<n;i+=100){std::vector<Store::Mutation> batch;for(int j=i;j<i+100;++j)batch.push_back({order[j],values[order[j]]});db->begin();db->apply_batch_in(7,batch);db->commit();}
  const auto write_ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();db.reset();
  const auto reopen=Clock::now();db=open();const auto reopen_ms=std::chrono::duration<double,std::milli>(Clock::now()-reopen).count();
  std::uint64_t verified=0;
  for(const auto mode:{"key_order","random","hot_set"}){
    std::vector<int> requests(n);for(int i=0;i<n;++i)requests[i]=std::string(mode)=="key_order"?i:std::string(mode)=="random"?int(rng()%n):order[i%100];
    // The timer run has no profiling clocks. A second exact run measures I/O
    // calls and bytes separately; these are syscall bytes, not physical B.
    const auto at=Clock::now();for(auto key:requests){auto value=db->get_in(7,key);if(!value||*value!=values[key])throw std::runtime_error("row mismatch");++verified;}
    const auto ms=std::chrono::duration<double,std::milli>(Clock::now()-at).count();
    profile::reset();profile::set_enabled(true);for(auto key:requests){auto value=db->get_in(7,key);if(!value||*value!=values[key])throw std::runtime_error("profile row mismatch");}profile::set_enabled(false);const auto p=profile::snapshot();
    std::cout<<"{\"mode\":\""<<mode<<"\",\"corpus\":\""<<argv[3]<<"\",\"cache_budget\":"<<budget<<",\"T_ms\":"<<ms<<",\"pread_calls\":"<<p.pread.calls<<",\"pread_bytes\":"<<p.pread.bytes<<",\"write_ms\":"<<write_ms<<",\"reopen_ms\":"<<reopen_ms<<",\"file_bytes\":"<<std::filesystem::file_size(path)<<",\"exact\":true}"<<std::endl;
  }
  if(verified!=30000)throw std::runtime_error("verification count");
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
