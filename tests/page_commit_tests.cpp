#include "journal_harness.hpp"
#include "../src/storage/page_checksum.hpp"
#include <fstream>
#include <cstring>
#include <array>
#include <algorithm>
#include <iostream>
#include <random>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
class FramedStore:public Store {public:FramedStore(const std::filesystem::path&p,CommitSyncPolicy policy):Store(p,0,true,0,65536,policy,true,true,true,false,true,true,true){}};
using Page=std::array<unsigned char,4096>;
void put(unsigned char*p,std::uint64_t v,unsigned n=8){for(unsigned i=0;i<n;++i)p[i]=static_cast<unsigned char>(v>>(8*i));}
std::uint32_t crc(std::string_view s){return ~detail::page_crc32_span(0xffffffffU,reinterpret_cast<const unsigned char*>(s.data()),s.size());}
void legacy_fixture(const std::filesystem::path&p){Page a{},b{};std::array<unsigned char,48> h{};const std::string payload="legacy payload";
 std::memcpy(h.data(),"JREC",4);put(h.data()+8,7);put(h.data()+16,9);put(h.data()+32,payload.size());put(h.data()+40,crc(payload),4);put(h.data()+44,crc({reinterpret_cast<char*>(h.data()),44}),4);
 std::memcpy(a.data(),"MVDBJL08",8);put(a.data()+8,1);put(a.data()+16,8192);put(a.data()+24,8192+h.size()+payload.size());put(a.data()+32,detail::page_checksum(a,32),4);
 std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<char*>(a.data()),a.size());f.write(reinterpret_cast<char*>(b.data()),b.size());f.write(reinterpret_cast<char*>(h.data()),h.size());f<<payload;CHECK(f.good());
}
#if defined(__linux__)
static bool armed=false,partial_pending=false;static int writes=0,syncs=0,fail_write=0,fail_sync=0,fault_mode=0;
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t at){
 if(armed){if(partial_pending){partial_pending=false;errno=EIO;return -1;}
  if(++writes==fail_write){if(fault_mode==1){partial_pending=true;return __real_pwrite(fd,p,std::min(n/2,size_t(17)),at);}if(fault_mode==2){errno=EINTR;return -1;}if(fault_mode==3)return 0;errno=EIO;return -1;}}
 return __real_pwrite(fd,p,n,at);
}
extern "C" int __wrap_fsync(int fd){if(armed&&++syncs==fail_sync){errno=EIO;return -1;}return __real_fsync(fd);}
void faults(const std::filesystem::path& dir){
 for(auto policy:{CommitSyncPolicy::Ordered,CommitSyncPolicy::ProcessCrash}){
  const auto seed=dir/"fault-seed";std::filesystem::remove(seed);
  {FramedStore db(seed,policy);db.begin();db.put_in(7,1,"prior");db.commit();}
  const int steps=policy==CommitSyncPolicy::Ordered?2:1;
  for(int kind=0;kind<2;++kind)for(int point=1;point<=steps;++point)for(int mode=0;mode<4;++mode){
   if(kind&&mode)continue;const auto path=dir/"fault-case";std::filesystem::copy_file(seed,path,std::filesystem::copy_options::overwrite_existing);
   bool failed=false;
   {FramedStore db(path,policy);db.begin();db.put_in(7,2,"next");writes=syncs=0;fail_write=kind?0:point;fail_sync=kind?point:0;fault_mode=mode;partial_pending=false;armed=true;
    try{db.commit();}catch(const std::exception&){failed=true;}armed=false;partial_pending=false;
    CHECK(failed==(kind||mode!=2));if(failed){bool poisoned=false;try{db.get_in(7,1);}catch(const std::exception&){poisoned=true;}CHECK(poisoned);}
   }
   {Store db(path);CHECK(db.get_in(7,1)=="prior");const auto next=db.get_in(7,2);CHECK(!next||*next=="next");if(!failed)CHECK(next=="next");}
  }
 }
}
#endif
int main(){try{
 const auto dir=std::filesystem::path(std::getenv("MVDB_TEST_DIR")?std::getenv("MVDB_TEST_DIR"):"/tmp")/("page-commit-"+std::to_string(getpid()));std::filesystem::create_directories(dir);
 for(auto policy:{CommitSyncPolicy::Ordered,CommitSyncPolicy::ProcessCrash}){
  const auto path=dir/(policy==CommitSyncPolicy::Ordered?"ordered":"process");std::mt19937 rng(912);std::string payload(12007,'\0');for(auto& c:payload)c=char(rng());payload.replace(4040,5,"JFR10");
  {FramedStore db(path,policy);db.begin();db.put_in(7,1,payload);CHECK(db.get_in(7,1)==payload);db.commit();CHECK(std::filesystem::file_size(path)%4096==0);}
  auto committed=std::filesystem::file_size(path);
  auto pid=fork();CHECK(pid>=0);if(!pid){FramedStore db(path,policy);db.begin();for(int i=0;i<10;++i)db.put_in(7,i+2,payload);_exit(0);}int status=0;CHECK(waitpid(pid,&status,0)==pid&&WEXITSTATUS(status)==0);
  CHECK(std::filesystem::file_size(path)>committed);
  {FramedStore db(path,policy);CHECK(db.get_in(7,1)==payload);CHECK(!db.get_in(7,2));CHECK(std::filesystem::file_size(path)==committed);db.begin();db.put_in(7,2,"after orphan");db.commit();db.begin();db.commit();}
  {Store db(path);CHECK(db.get_in(7,1)==payload);CHECK(db.get_in(7,2)=="after orphan");}
  // An incomplete physical suffix can contain arbitrary marker-like bytes.
  {std::ofstream out(path,std::ios::binary|std::ios::app);out<<"JFR10"<<std::string(133,'x');}
  {FramedStore db(path,policy);CHECK(db.get_in(7,1)==payload);db.begin();db.erase_in(7,1);db.commit();}
  {Store db(path);CHECK(!db.get_in(7,1));CHECK(db.get_in(7,2)=="after orphan");}
 }
 const auto legacy=dir/"legacy";legacy_fixture(legacy);{Store db(legacy);CHECK(db.get_in(7,9)=="legacy payload");db.begin();db.put_in(7,10,"new legacy write");db.commit();}
 {Store db(legacy);CHECK(db.get_in(7,9)=="legacy payload");CHECK(db.get_in(7,10)=="new legacy write");}
 // A complete footer with damaged committed data is rejected, never rolled
 // back silently. A torn footer itself can fall back to a previous commit.
 const auto damaged=dir/"damaged";{FramedStore db(damaged,CommitSyncPolicy::ProcessCrash);db.begin();db.put_in(7,1,"payload");db.commit();}
 {std::fstream f(damaged,std::ios::in|std::ios::out|std::ios::binary);f.seekp(8192+48);f.put('X');}
 bool rejected=false;try{Store db(damaged);}catch(const std::exception&){rejected=true;}CHECK(rejected);
 #if defined(__linux__)
 faults(dir);
#endif
 std::filesystem::remove_all(dir);std::cout<<"page commit: both policies, pending reads, orphan/torn-tail recovery, reopen, legacy read/write and corruption passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
