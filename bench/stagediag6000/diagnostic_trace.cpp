#include <cstdint>
#include <atomic>
#include <thread>
#include <chrono>
#include <fcntl.h>
#include <cstdio>
#include <cerrno>
#include <ctime>
#include <unistd.h>
#include <sys/syscall.h>
namespace {
std::uint64_t ns(clockid_t clock){timespec t{};clock_gettime(clock,&t);return std::uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;}
struct Stats {std::uint64_t start=0,cpu=0,sync=0,sync_cpu=0,writeback=0,writeback_count=0,pwrite=0,pread=0,syncs=0,writes=0,reads=0;};
thread_local Stats stats;
thread_local std::uint64_t stage_ns[8]{};
thread_local std::uint64_t sequence=0;
void emit(const char* data,int size){const int saved=errno;for(int p=0;p<size;){auto n=syscall(SYS_write,2,data+p,size-p);if(n<0&&errno==EINTR)continue;if(n<=0)break;p+=int(n);}errno=saved;}
void slow(const char* kind,int fd,std::uint64_t begin,std::uint64_t wall,std::uint64_t cpu){if(wall<2000000)return;char b[384];const auto n=snprintf(b,sizeof b,"{\"trace\":\"syscall\",\"seq\":%llu,\"kind\":\"%s\",\"fd\":%d,\"start_ns\":%llu,\"wall_ns\":%llu,\"cpu_ns\":%llu}\n",(unsigned long long)sequence,kind,fd,(unsigned long long)begin,(unsigned long long)wall,(unsigned long long)cpu);emit(b,n);}
}
extern "C" void mvdb_diag_begin(){++sequence;stats={};for(auto& v:stage_ns)v=0;stats.start=ns(CLOCK_MONOTONIC);stats.cpu=ns(CLOCK_THREAD_CPUTIME_ID);}
extern "C" std::uint64_t mvdb_stage_clock(){return ns(CLOCK_THREAD_CPUTIME_ID);}
extern "C" void mvdb_stage_add(unsigned stage,std::uint64_t elapsed){if(stage<8)stage_ns[stage]+=elapsed;}
extern "C" void mvdb_diag_end(){char sb[512];int sn=snprintf(sb,sizeof sb,"{\"trace\":\"stages\",\"seq\":%llu,\"json_ns\":%llu,\"parse_ns\":%llu,\"catalog_ns\":%llu,\"bind_ns\":%llu,\"duplicate_ns\":%llu,\"mutation_ns\":%llu,\"apply_ns\":%llu,\"commit_ns\":%llu}\n",(unsigned long long)sequence,(unsigned long long)stage_ns[0],(unsigned long long)stage_ns[1],(unsigned long long)stage_ns[2],(unsigned long long)stage_ns[3],(unsigned long long)stage_ns[4],(unsigned long long)stage_ns[5],(unsigned long long)stage_ns[6],(unsigned long long)stage_ns[7]);emit(sb,sn);const auto wall=ns(CLOCK_MONOTONIC)-stats.start,cpu=ns(CLOCK_THREAD_CPUTIME_ID)-stats.cpu;char b[640];const auto n=snprintf(b,sizeof b,"{\"trace\":\"request\",\"seq\":%llu,\"start_ns\":%llu,\"wall_ns\":%llu,\"cpu_ns\":%llu,\"fsync_ns\":%llu,\"fsync_cpu_ns\":%llu,\"fsync_count\":%llu,\"writeback_ns\":%llu,\"writeback_count\":%llu,\"pwrite_ns\":%llu,\"pwrite_count\":%llu,\"pread_ns\":%llu,\"pread_count\":%llu}\n",(unsigned long long)sequence,(unsigned long long)stats.start,(unsigned long long)wall,(unsigned long long)cpu,(unsigned long long)stats.sync,(unsigned long long)stats.sync_cpu,(unsigned long long)stats.syncs,(unsigned long long)stats.writeback,(unsigned long long)stats.writeback_count,(unsigned long long)stats.pwrite,(unsigned long long)stats.writes,(unsigned long long)stats.pread,(unsigned long long)stats.reads);emit(b,n);}
namespace {
// Diagnostic-only observer of this process's own fsync thread. Sampling does
// not modify any storage operation; identical instrumentation in both images.
struct WaitMonitor {
 std::atomic<std::uint64_t> started{0},seq{0};std::atomic<bool> stop{false};
 int tid=int(syscall(SYS_gettid));std::thread worker;
 WaitMonitor():worker([this]{run();}){}
 ~WaitMonitor(){stop=true;started=1;started.notify_one();worker.join();}
 void run(){char path[96];snprintf(path,sizeof path,"/proc/self/task/%d/wchan",tid);
  while(!stop.load()){
   auto begin=started.load();if(!begin){started.wait(0);continue;}
   std::this_thread::sleep_for(std::chrono::milliseconds(10));
   if(stop.load())break;if(started.load()!=begin)continue;
   const auto elapsed=ns(CLOCK_MONOTONIC)-begin;if(elapsed<10000000)continue;
   char channel[160]="unavailable";int fd=open(path,O_RDONLY|O_CLOEXEC);
   if(fd>=0){auto n=read(fd,channel,sizeof(channel)-1);close(fd);if(n>=0)channel[n]=0;}
   if(started.load()!=begin)continue;
   // proc wchan names consist of alphanumerics and underscores; sanitize any
   // unexpected bytes rather than emitting malformed diagnostic JSON.
   for(char* p=channel;*p;++p)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'))*p='_';
   char b[384];int n=snprintf(b,sizeof b,"{\"trace\":\"fsync_wait\",\"seq\":%llu,\"start_ns\":%llu,\"elapsed_ns\":%llu,\"wchan\":\"%s\"}\n",(unsigned long long)seq.load(),(unsigned long long)begin,(unsigned long long)elapsed,channel);emit(b,n);
  }
 }
};
WaitMonitor& monitor(){static WaitMonitor m;return m;}
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){auto& m=monitor();m.seq=sequence;const auto a=ns(CLOCK_MONOTONIC),c=ns(CLOCK_THREAD_CPUTIME_ID);m.started=a;m.started.notify_one();const auto r=__real_fsync(fd);m.started=0;const int saved=errno;const auto wall=ns(CLOCK_MONOTONIC)-a,cpu=ns(CLOCK_THREAD_CPUTIME_ID)-c;stats.sync+=wall;stats.sync_cpu+=cpu;++stats.syncs;slow("fsync",fd,a,wall,cpu);errno=saved;return r;}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t size,off_t offset){const auto a=ns(CLOCK_MONOTONIC);const auto r=__real_pwrite(fd,data,size,offset);const int saved=errno;const auto wall=ns(CLOCK_MONOTONIC)-a;stats.pwrite+=wall;++stats.writes;slow("pwrite",fd,a,wall,0);errno=saved;return r;}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t size,off_t offset){const auto a=ns(CLOCK_MONOTONIC);const auto r=__real_pread(fd,data,size,offset);const int saved=errno;const auto wall=ns(CLOCK_MONOTONIC)-a;stats.pread+=wall;++stats.reads;slow("pread",fd,a,wall,0);errno=saved;return r;}

extern "C" int __real_sync_file_range(int,off_t,off_t,unsigned);
extern "C" int __wrap_sync_file_range(int fd,off_t offset,off_t count,unsigned flags){const auto a=ns(CLOCK_MONOTONIC),c=ns(CLOCK_THREAD_CPUTIME_ID);const auto r=__real_sync_file_range(fd,offset,count,flags);const int saved=errno;const auto wall=ns(CLOCK_MONOTONIC)-a,cpu=ns(CLOCK_THREAD_CPUTIME_ID)-c;stats.writeback+=wall;++stats.writeback_count;slow("writeback",fd,a,wall,cpu);errno=saved;return r;}
