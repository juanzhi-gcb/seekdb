#include "journal_harness.hpp"
#include "../src/storage/page_checksum.hpp"
#include <map>
#include <random>
#include <fstream>
#include <cstring>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
using namespace minivectordb;
using Page=std::array<unsigned char,4096>;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
class BatchStore:public Store{public:BatchStore(const std::filesystem::path&p,CommitSyncPolicy policy=CommitSyncPolicy::ProcessCrash):Store(p,0,true,0,65536,policy,true,true,true,false,true,true,true){}};
void put(unsigned char*p,std::uint64_t v,unsigned n=8){for(unsigned i=0;i<n;++i)p[i]=static_cast<unsigned char>(v>>(8*i));}
std::uint32_t crc(std::string_view s){return ~detail::page_crc32_span(0xffffffffU,reinterpret_cast<const unsigned char*>(s.data()),s.size());}
using Model=std::map<std::int64_t,std::string>;
Model rows(Store& s,std::uint64_t ns=7){Model out;s.scan_in(ns,[&](auto k,auto v){CHECK(out.emplace(k,v).second);});for(auto&[k,v]:out)CHECK(s.get_in(ns,k)==v);return out;}
Page page(const std::filesystem::path&path,std::uint64_t off){Page p{};std::ifstream f(path,std::ios::binary);f.seekg(off);f.read(reinterpret_cast<char*>(p.data()),p.size());CHECK(f.good());return p;}
void savepage(const std::filesystem::path&path,std::uint64_t off,const Page&p){std::fstream f(path,std::ios::in|std::ios::out|std::ios::binary);f.seekp(off);f.write(reinterpret_cast<const char*>(p.data()),p.size());CHECK(f.good());}
void reframe(Page&p){put(p.data()+4048+36,crc({reinterpret_cast<char*>(p.data()),4048}),4);put(p.data()+4048+40,crc({reinterpret_cast<char*>(p.data()+4048),40}),4);}
void legacy10(const std::filesystem::path&path){
 Page root{},data{};std::memcpy(root.data(),"MVDBJL10",8);put(root.data()+24,8192);put(root.data()+32,detail::page_checksum(root,32),4);
 std::string payload="old J10 payload";auto*h=data.data();std::memcpy(h,"JREC",4);put(h+8,7);put(h+16,1);put(h+32,payload.size());put(h+40,crc(payload),4);put(h+44,crc({reinterpret_cast<char*>(h),44}),4);std::memcpy(h+48,payload.data(),payload.size());
 auto*f=data.data()+4048;std::memcpy(f,"JFR10",5);f[5]=1;put(f+8,1);put(f+16,8192);put(f+24,8192+4048);put(f+32,48+payload.size(),4);reframe(data);
 std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char*>(root.data()),4096);out.write(reinterpret_cast<char*>(root.data()),4096);out.write(reinterpret_cast<char*>(data.data()),4096);CHECK(out.good());
}
#if defined(__linux__)
static bool armed=false,partial=false;static int calls=0,syncs=0,fail_at=0,fault=0;
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pwrite(int fd,const void*p,size_t n,off_t at){if(armed){if(partial){partial=false;errno=EIO;return -1;}if(++calls==fail_at){if(fault==1){partial=true;return __real_pwrite(fd,p,std::min(n/2,size_t(17)),at);}if(fault==2){errno=EINTR;return -1;}if(fault==3)return 0;errno=EIO;return -1;}}return __real_pwrite(fd,p,n,at);}
extern "C" int __wrap_fsync(int fd){if(armed)++syncs;return __real_fsync(fd);}
void fault_cases(const std::filesystem::path&dir){
 std::mt19937 rng(923);std::vector<Store::Mutation> batch;for(int i=0;i<128;++i){std::string s(2048,'\0');for(auto&c:s)c=char(rng());batch.push_back({i,s});}
 for(auto policy:{CommitSyncPolicy::ProcessCrash,CommitSyncPolicy::Ordered}){
  const auto seed=dir/"seed";std::filesystem::remove(seed);{BatchStore db(seed,policy);db.begin();db.put_in(7,-1,"durable");db.commit();}
  int count=0;const auto measured=dir/"measure";std::filesystem::copy_file(seed,measured,std::filesystem::copy_options::overwrite_existing);
  {BatchStore db(measured,policy);db.begin();db.put_in(7,-2,"earlier");calls=0;fail_at=0;armed=true;db.apply_batch_in(7,batch);armed=false;count=calls;syncs=0;armed=true;db.commit();armed=false;CHECK(syncs==(policy==CommitSyncPolicy::Ordered?2:1));}CHECK(count>0);
  for(int mode=0;mode<4;++mode)for(int n=1;n<=count;++n){const auto path=dir/"fault";std::filesystem::copy_file(seed,path,std::filesystem::copy_options::overwrite_existing);
   {BatchStore db(path,policy);db.begin();db.put_in(7,-2,"earlier");const auto before=rows(db);const auto sp=db.savepoint();calls=0;fail_at=n;fault=mode;partial=false;armed=true;bool failed=false;try{db.apply_batch_in(7,batch);}catch(const std::exception&){failed=true;}armed=false;partial=false;CHECK(failed==(mode!=2));if(failed)CHECK(rows(db)==before);db.restore(sp);CHECK(rows(db)==before);db.apply_batch_in(7,batch);for(auto&c:batch)CHECK(db.get_in(7,c.key)==c.value);db.restore(sp);CHECK(rows(db)==before);db.commit();}
   {Store db(path);CHECK((rows(db)==Model{{-2,"earlier"},{-1,"durable"}}));}
  }
 }
}
#endif
