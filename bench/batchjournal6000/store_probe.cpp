#include "minivectordb/store.hpp"
#include <vector>
#include <random>
#include <map>
#include <cstring>
#include <iostream>
#include <chrono>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
using namespace minivectordb;
std::string payload(std::mt19937& random,unsigned corpus){std::string s(803,'x');for(unsigned d=0;d<128;++d){float value=corpus?float(double(random())/4294967296.):float(int(random()%4097)-2048)/128.f;std::memcpy(s.data()+25+d*4,&value,4);}for(unsigned i=0;i<25;++i)s[i]=char(random());return s;}
double ms(std::chrono::steady_clock::time_point a,std::chrono::steady_clock::time_point b){return std::chrono::duration<double,std::milli>(b-a).count();}
std::uint64_t io_counter(const char* key){std::ifstream f("/proc/self/io");std::string name;std::uint64_t n;while(f>>name>>n)if(name==key)return n;return 0;}
int main(int argc,char**argv){try{if(argc!=3)throw std::runtime_error("path corpus");const unsigned corpus=unsigned(std::stoul(argv[2]));std::mt19937 random(91);std::map<std::int64_t,std::string> expected;double apply_ms=0,commit_ms=0;
 {Store db(argv[1],0,false,262144,0,CommitSyncPolicy::ProcessCrash,false,true,false,false,false,false,true);
 for(int batch=0;batch<80;++batch){std::vector<Store::Mutation> changes;for(int i=0;i<100;++i){const auto key=batch<40?batch*100+i:(batch-40)*100+i;changes.push_back({key,payload(random,corpus)});}auto before=std::chrono::steady_clock::now();db.begin();db.apply_batch_in(7,changes);auto applied=std::chrono::steady_clock::now();if(batch>=40 && batch%3==0)db.rollback();else{db.commit();for(auto& c:changes)expected[c.key]=*c.value;}auto after=std::chrono::steady_clock::now();apply_ms+=std::chrono::duration<double,std::milli>(applied-before).count();commit_ms+=std::chrono::duration<double,std::milli>(after-applied).count();}
 for(const auto&[k,v]:expected)if(db.get_in(7,k)!=v)throw std::runtime_error("live mismatch");}
 {Store db(argv[1],0,false,262144,0,CommitSyncPolicy::ProcessCrash,false,true,false,false,false,false,true);for(const auto&[k,v]:expected)if(db.get_in(7,k)!=v)throw std::runtime_error("restart mismatch");}
 for(auto budget:{std::size_t(0),std::size_t(65536)}){
  const auto start=std::chrono::steady_clock::now();Store db(argv[1],0,false,262144,budget,CommitSyncPolicy::ProcessCrash,false,true,false,false,false,false,true);const auto ready=std::chrono::steady_clock::now();
  for(bool cold:{false,true}){
   const auto count=cold?256U:10000U;std::mt19937 keys(445);std::uint64_t hash=0;
   const int fd=::open(argv[1],O_RDONLY);if(fd<0)throw std::runtime_error("open for read advice");
   const auto calls=io_counter("syscr:"),read_bytes=io_counter("rchar:"),physical=io_counter("read_bytes:");const auto begin=std::chrono::steady_clock::now();
   for(unsigned i=0;i<count;++i){
#if defined(POSIX_FADV_DONTNEED)
    if(cold)::posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED);
#endif
    const auto key=std::int64_t(keys()%expected.size());const auto value=db.get_in(7,key);if(!value||*value!=expected.at(key))throw std::runtime_error("random read mismatch");hash+=value->size()+static_cast<unsigned char>((*value)[23]);
   }
   const auto end=std::chrono::steady_clock::now();::close(fd);
   std::cout<<"{\"scope\":\"storage-only random access, advice may be ignored; not official T/B/M\",\"cache_bytes\":"<<budget<<",\"cold_advice\":"<<cold<<",\"reads\":"<<count<<",\"read_ms\":"<<ms(begin,end)<<",\"reopen_ms\":"<<ms(start,ready)<<",\"syscr\":"<<io_counter("syscr:")-calls<<",\"rchar\":"<<io_counter("rchar:")-read_bytes<<",\"physical_read_bytes\":"<<io_counter("read_bytes:")-physical<<",\"hash\":"<<hash<<"}\n";
  }
 }
 std::cout<<"{\"apply_ms\":"<<apply_ms<<",\"commit_ms\":"<<commit_ms<<",\"rows\":"<<expected.size()<<",\"file_bytes\":"<<std::filesystem::file_size(argv[1])<<"}\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
