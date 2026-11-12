#include "minivectordb/store.hpp"
#include <random>
#include <iostream>
#include <cerrno>
#include <unistd.h>
using namespace minivectordb;
static int fault=0,stage=0;static unsigned combined=0;
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t offset){
  if(n>4096)++combined;
  if(fault && n>4096){
    if(stage++==0){if(fault==1){errno=EINTR;return -1;}return __real_pwrite(fd,p,123,offset);}
    if(fault==3){errno=EIO;return -1;}
    if(fault==4)return 0;
  }
  return __real_pwrite(fd,p,n,offset);
}
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
int main(){try{
  const auto root=std::filesystem::path(std::getenv("MVDB_TEST_DIR")?std::getenv("MVDB_TEST_DIR"):"/tmp")/("writecombine-"+std::to_string(getpid()));std::filesystem::create_directories(root);
  std::mt19937 rng(20261112);std::vector<Store::Mutation> rows;
  for(int i=0;i<100;++i){std::string value(i%17==0?5000:512,'\0');for(char&c:value)c=char(rng());rows.push_back({i,std::move(value)});}
  for(int mode:{0,1,2,3,4}){
    auto path=root/std::to_string(mode);
    {Store db(path,0,true,0,0,CommitSyncPolicy::Ordered,true,true,true,false,true,true);db.begin();db.put(-1,"durable");db.commit();db.begin();
      stage=0;fault=mode;bool failed=false;
      try{db.apply_batch_in(0,rows);}catch(const std::exception&){failed=true;}fault=0;
      CHECK(failed==(mode>=3));CHECK(db.get(-1)=="durable");
      if(failed){for(const auto&r:rows)CHECK(!db.get(r.key));db.apply_batch_in(0,rows);}
      for(const auto&r:rows)CHECK(db.get(r.key)==r.value);db.commit();
    }
    {Store db(path);for(const auto&r:rows)CHECK(db.get(r.key)==r.value);CHECK(db.get(-1)=="durable");}
  }
  CHECK(combined>0);std::filesystem::remove_all(root);std::cout<<"coalesced writes, EINTR, short/zero write, EIO, retry and reopen passed; calls="<<combined<<'\n';
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
