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
int main(){try{
 const auto dir=std::filesystem::path(std::getenv("MVDB_TEST_DIR")?std::getenv("MVDB_TEST_DIR"):"/tmp")/("batch-journal-"+std::to_string(getpid()));std::filesystem::create_directories(dir);
 for(auto policy:{CommitSyncPolicy::ProcessCrash,CommitSyncPolicy::Ordered}){
  const auto path=dir/(policy==CommitSyncPolicy::Ordered?"ordered":"process");Model model;
  {BatchStore db(path,policy);db.begin();std::vector<Store::Mutation> batch;for(int i=0;i<128;++i){batch.push_back({INT64_MIN+i,i%3?std::string(80,char(i)):std::string()});model[batch.back().key]=*batch.back().value;}db.apply_batch_in(7,batch);CHECK(rows(db)==model);auto full=db.savepoint();
   std::vector<Store::Mutation> changes{{INT64_MIN,"updated"},{INT64_MIN+1,std::nullopt},{INT64_MIN+2,""}};db.apply_batch_in(7,changes);const auto changed=rows(db);const auto future=db.savepoint();db.restore(full);CHECK(rows(db)==model);db.restore(future);CHECK(rows(db)==changed);db.restore(full);
   // A wide range and a single large value must use ordinary JREC records.
   std::vector<Store::Mutation> wide{{0,"zero"},{INT64_MAX,"max"}};db.apply_batch_in(7,wide);model[0]="zero";model[INT64_MAX]="max";db.put_in(7,100000,std::string(530000,'z'));model[100000]=std::string(530000,'z');
   db.apply_batch_in(8,batch);std::vector<Store::Mutation> limit{{1,std::string(Store::max_batch_bytes,'q')},{2,""}};db.apply_batch_in(9,limit);CHECK(db.get_in(9,1)==limit[0].value&&db.get_in(9,2)==limit[1].value);db.commit();CHECK(rows(db)==model);
   db.begin();db.apply_batch_in(7,changes);auto expected=rows(db);Model observed;bool first=true;db.scan_in(7,[&](auto k,auto v){observed[k]=v;if(first){first=false;db.rollback();db.begin();db.apply_batch_in(7,batch);db.rollback();}});CHECK(observed==expected);CHECK(rows(db)==model);
  }
  {Store db(path);CHECK(rows(db)==model);CHECK(db.get_in(9,1)==std::string(Store::max_batch_bytes,'q'));CHECK(db.get_in(9,2)=="");}
  auto pid=fork();CHECK(pid>=0);if(!pid){BatchStore db(path,policy);db.begin();std::vector<Store::Mutation> batch;for(int i=0;i<100;++i)batch.push_back({i,std::string(1000,char(i))});db.apply_batch_in(7,batch);_exit(0);}int status=0;CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  {BatchStore db(path,policy);CHECK(rows(db)==model);db.begin();std::vector<Store::Mutation> b{{2,"after orphan"},{3,"more"}};db.apply_batch_in(7,b);db.commit();model[2]="after orphan";model[3]="more";}
  {Store db(path);CHECK(rows(db)==model);CHECK(db.get_in(9,1)==std::string(Store::max_batch_bytes,'q'));CHECK(db.get_in(9,2)=="");}
 }
 const auto legacy=dir/"old10";legacy10(legacy);{Store db(legacy);CHECK(db.get_in(7,1)=="old J10 payload");db.begin();std::vector<Store::Mutation> b{{2,"two"},{3,"three"}};db.apply_batch_in(7,b);db.commit();}{Store db(legacy);CHECK(db.get_in(7,1)=="old J10 payload"&&db.get_in(7,3)=="three");}CHECK(!std::memcmp(page(legacy,0).data(),"MVDBJL10",8));CHECK(!std::memcmp(page(legacy,12288).data(),"JREC",4));
 const auto seed=dir/"corrupt-seed";{BatchStore db(seed);db.begin();std::vector<Store::Mutation> b{{10,"first"},{11,"second"},{12,"third"}};db.apply_batch_in(7,b);db.commit();}const auto original=page(seed,8192);CHECK(!std::memcmp(original.data(),"JBA3",4));
 // Recompute the outer frame CRC to exercise the inner header/directory/payload checks.
 for(auto pos:{0,4,8,16,24,32,36,40,44,48,50,52,56,64,72,80}){const auto p=dir/"corrupt";std::filesystem::copy_file(seed,p,std::filesystem::copy_options::overwrite_existing);auto bytes=original;bytes[pos]^=1;reframe(bytes);savepage(p,8192,bytes);bool rejected=false;try{Store db(p);(void)rows(db);}catch(const std::exception&){rejected=true;}CHECK(rejected);}
 {const auto p=dir/"bad-slot";std::filesystem::copy_file(seed,p);auto bytes=original;put(bytes.data()+4048+16,(std::uint64_t(1)<<63)|(8192ULL<<15)|(2ULL<<7)|127);reframe(bytes);savepage(p,8192,bytes);bool rejected=false;try{Store db(p);}catch(const std::exception&){rejected=true;}CHECK(rejected);}
#if defined(__linux__)
 fault_cases(dir);std::cout<<"native pwrite injection and fsync-count checks passed\n";
#endif
 std::filesystem::remove_all(dir);std::cout<<"J11 batch model, signed keys, deletions, snapshots, orphan recovery, J10 writes and corruption passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
