#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
using Clock=std::chrono::steady_clock;
static double ms(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
static void writeall(int fd,const void*p,size_t n,uint64_t off){size_t done=0;while(done<n){auto k=pwrite(fd,(const char*)p+done,n-done,off+done);if(k<0){if(errno==EINTR)continue;throw std::runtime_error("pwrite");}if(!k)throw std::runtime_error("zero write");done+=k;}}
static void syncfile(int fd,bool data){while((data?fdatasync(fd):fsync(fd))){if(errno!=EINTR)throw std::runtime_error("sync");}}
struct Mode{const char*name;int reserve;bool zero;bool data;};
int main(int argc,char**argv){if(argc!=2)return 2;std::filesystem::create_directories(argv[1]);
 std::array<Mode,6> modes={{{"append-fsync",0,false,false},{"append-fdatasync",0,false,true},{"allocate256-fdatasync",262144,false,true},{"zero64-fdatasync",65536,true,true},{"zero256-fdatasync",262144,true,true},{"zero256-fsync",262144,true,false}}};
 std::mt19937 rng(441);std::vector<unsigned char> data(40960),zeros(262144);for(auto&v:data)v=rng();
 std::array<int,6> order={0,1,2,3,4,5};
 for(int round=0;round<5;++round){std::shuffle(order.begin(),order.end(),rng);
  for(int m:order){auto mode=modes[m];std::string path=std::string(argv[1])+"/"+mode.name+"-"+std::to_string(round);int fd=open(path.c_str(),O_CREAT|O_EXCL|O_RDWR,0600);if(fd<0)return 3;
   uint64_t allocated=0,offset=0;std::vector<double> times,syncs;double init_ms=0;auto whole=Clock::now();
   for(int i=0;i<200;++i){auto begin=Clock::now();if(mode.reserve&&offset+data.size()>allocated){auto init=Clock::now();uint64_t next=(offset+data.size()+mode.reserve-1)/mode.reserve*mode.reserve;if(mode.zero)writeall(fd,zeros.data(),next-allocated,allocated);else if(posix_fallocate(fd,allocated,next-allocated))throw std::runtime_error("allocate");allocated=next;init_ms+=ms(init);}
    writeall(fd,data.data(),data.size(),offset);auto sync=Clock::now();syncfile(fd,mode.data);syncs.push_back(ms(sync));times.push_back(ms(begin));offset+=data.size();
   }
   auto total=ms(whole);close(fd);fd=open(path.c_str(),O_RDONLY);std::vector<unsigned char> check(data.size());for(uint64_t off=0;off<offset;off+=data.size()){if(pread(fd,check.data(),check.size(),off)!=(ssize_t)check.size()||check!=data)throw std::runtime_error("reopen verification");}close(fd);
   printf("{\"mode\":\"%s\",\"round\":%d,\"total_ms\":%.6f,\"initialization_ms\":%.6f,\"file_bytes\":%llu,\"requests_ms\":[",mode.name,round,total,init_ms,(unsigned long long)std::filesystem::file_size(path));for(size_t i=0;i<times.size();++i)printf("%s%.6f",i?",":"",times[i]);printf("],\"sync_ms\":[");for(size_t i=0;i<syncs.size();++i)printf("%s%.6f",i?",":"",syncs[i]);puts("]}");fflush(stdout);std::filesystem::remove(path);
  }
 }
}
