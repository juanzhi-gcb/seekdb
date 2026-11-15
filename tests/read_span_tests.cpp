#include "minivectordb/profile.hpp"
#include "../src/storage/journal_store.hpp"
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
using namespace minivectordb;
using minivectordb::detail::JournalStore;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
struct DB {
  int fd;std::unique_ptr<JournalStore> db;
  DB(const std::filesystem::path& p,bool create,std::size_t budget,CommitSyncPolicy policy) {
    fd=::open(p.c_str(),O_RDWR|O_CREAT|(create?O_TRUNC:0),0600);CHECK(fd>=0);
    try{db=std::make_unique<JournalStore>(fd,create,budget,policy);}catch(...){::close(fd);throw;}
  }
  ~DB(){db.reset();::close(fd);}
};
void put32(unsigned char* p,std::uint32_t x){for(unsigned i=0;i<4;++i)p[i]=static_cast<unsigned char>(x>>(8*i));}
int main(){try{
  auto root=std::filesystem::temp_directory_path()/("read-span-"+std::to_string(getpid()));std::filesystem::create_directories(root);
  unsigned cases=0;std::size_t contiguous=0,crossing=0;
  for(std::size_t budget:{0u,8192u,65536u})for(auto policy:{CommitSyncPolicy::Ordered,CommitSyncPolicy::ProcessCrash}){
    const auto path=root/std::to_string(cases++);std::map<std::int64_t,std::string> expected;std::mt19937 rng(8162);
    {DB f(path,true,budget,policy);f.db->begin();
      for(int batch=0;batch<8;++batch){std::vector<Store::Mutation> changes;
        for(int row=0;row<48;++row){const auto key=batch*48+row;std::string value((row%12==0)?0:40+rng()%3200,'x');if(row%3)for(auto& c:value)c=static_cast<char>(rng());expected[key]=value;changes.push_back({key,value});}
        f.db->apply(7,changes);
        // Includes pending frame reads before the commit footer is written.
        for(const auto& change:changes)CHECK(f.db->value(7,change.key)==change.value);
        f.db->commit();f.db->begin();
      }
      for(auto [key,token]:f.db->snapshot(7)){
        CHECK((token>>62)==3);const auto at=token&((1ULL<<41)-1);const auto n=((token>>41)&((1ULL<<20)-1))+4;
        if(n<=4048-(at-8192)%4048)++contiguous;else ++crossing;
      }
      // Force many cache collisions and verify each returned string is owned.
      std::vector<std::string> retained;
      for(unsigned n=0;n<2000;++n){auto key=std::int64_t(rng()%384);auto value=f.db->value(7,key);CHECK(value==expected[key]);if(n<20)retained.push_back(*value);}
      CHECK(!retained.empty());
      f.db->begin();const auto point=f.db->head();std::vector<Store::Mutation> change{{0,"replacement"},{1,std::nullopt},{2,std::string(3000,'z')}};f.db->apply(7,change);CHECK(f.db->value(7,0)=="replacement");CHECK(!f.db->value(7,1));f.db->restore(point);for(int key=0;key<3;++key)CHECK(f.db->value(7,key)==expected[key]);f.db->rollback();
    }
    {DB f(path,false,budget,policy);for(const auto&[key,value]:expected)CHECK(f.db->value(7,key)==value);}
  }
  CHECK(contiguous>0&&crossing>0);
  // Repair the frame checksums after corrupting a row's stored CRC. A frame
  // check is not a substitute for the independent record checksum.
  {const auto path=root/"record-crc";DB f(path,true,0,CommitSyncPolicy::Ordered);f.db->begin();std::vector<Store::Mutation> changes{{1,std::string(100,'a')},{2,std::string(100,'b')}};f.db->apply(9,changes);f.db->commit();
    const auto at=f.db->snapshot(9).at(0).second&((1ULL<<41)-1);const auto block=(at-8192)/4048,inside=(at-8192)%4048;std::array<unsigned char,4096> page;CHECK(::pread(f.fd,page.data(),page.size(),off_t(8192+block*4096))==4096);page[inside]^=1;
    put32(page.data()+4048+36,~detail::page_crc32_span(0xffffffffU,page.data(),4048));put32(page.data()+4048+40,~detail::page_crc32_span(0xffffffffU,page.data()+4048,40));CHECK(::pwrite(f.fd,page.data(),page.size(),off_t(8192+block*4096))==4096);
    bool rejected=false;try{(void)f.db->value(9,1);}catch(const std::runtime_error&){rejected=true;}CHECK(rejected);
  }
  // Direct-row reads must reject truncated frames before borrowing any bytes.
  for(std::size_t budget:{0u,65536u})for(unsigned length:{0u,1u,511u,4047u,4048u,4095u}){
    const auto path=root/("short-"+std::to_string(cases++));DB f(path,true,budget,CommitSyncPolicy::Ordered);f.db->begin();std::vector<Store::Mutation> changes{{1,std::string(100,'a')},{2,std::string(100,'b')}};f.db->apply(9,changes);f.db->commit();CHECK(::ftruncate(f.fd,off_t(8192+length))==0);bool rejected=false;try{(void)f.db->value(9,1);}catch(const std::runtime_error&){rejected=true;}CHECK(rejected);
  }
  std::filesystem::remove_all(root);std::cout<<"PASS borrowed frame reads: "<<contiguous<<" contiguous / "<<crossing<<" crossing records; cached/pending/restart/rollback/short-read and independent CRC checks\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
