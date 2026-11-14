#include "minivectordb/store.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace minivectordb;
int main(){try{
 const auto dir=std::filesystem::path("/tmp")/("frame-short-"+std::to_string(getpid()));std::filesystem::create_directories(dir);unsigned tested=0;
 for(size_t budget:{size_t(0),size_t(65536)})for(auto policy:{CommitSyncPolicy::Ordered,CommitSyncPolicy::ProcessCrash})for(unsigned length:{0u,1u,31u,48u,511u,2048u,4047u,4048u,4095u}){
  const auto path=dir/std::to_string(tested);
  {Store db(path,0,true,0,budget,policy,true,true,true,false,true,true,true);db.begin();db.put_in(7,1,std::string(1000,'v'));db.commit();
   std::filesystem::resize_file(path,8192+length);bool failed=false;
   try{(void)db.get_in(7,1);}catch(const std::runtime_error&){failed=true;}
   if(!failed)throw std::runtime_error("incomplete frame accepted at "+std::to_string(length));
  }++tested;
 }
 std::filesystem::remove_all(dir);std::cout<<tested<<" incomplete-frame reads rejected before payload access, both cache budgets and sync policies\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
