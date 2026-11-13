#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <ctime>
#include <unistd.h>
#include <sys/syscall.h>
namespace {
std::uint64_t ns(clockid_t clock){timespec t{};clock_gettime(clock,&t);return std::uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;}
struct Stats {std::uint64_t start=0,cpu=0,sync=0,sync_cpu=0,pwrite=0,pread=0,syncs=0,writes=0,reads=0;};
thread_local Stats stats;
thread_local std::uint64_t sequence=0;
void emit(const char* data,int size){const int saved=errno;for(int p=0;p<size;){auto n=syscall(SYS_write,2,data+p,size-p);if(n<0&&errno==EINTR)continue;if(n<=0)break;p+=int(n);}errno=saved;}
void slow(const char* kind,int fd,std::uint64_t begin,std::uint64_t wall,std::uint64_t cpu){if(wall<2000000)return;char b[384];const auto n=snprintf(b,sizeof b,"{\"trace\":\"syscall\",\"seq\":%llu,\"kind\":\"%s\",\"fd\":%d,\"start_ns\":%llu,\"wall_ns\":%llu,\"cpu_ns\":%llu}\n",(unsigned long long)sequence,kind,fd,(unsigned long long)begin,(unsigned long long)wall,(unsigned long long)cpu);emit(b,n);}
}
extern "C" void mvdb_diag_begin(){++sequence;stats={};stats.start=ns(CLOCK_MONOTONIC);stats.cpu=ns(CLOCK_THREAD_CPUTIME_ID);}
extern "C" void mvdb_diag_end(){const auto wall=ns(CLOCK_MONOTONIC)-stats.start,cpu=ns(CLOCK_THREAD_CPUTIME_ID)-stats.cpu;char b[640];const auto n=snprintf(b,sizeof b,"{\"trace\":\"request\",\"seq\":%llu,\"start_ns\":%llu,\"wall_ns\":%llu,\"cpu_ns\":%llu,\"fsync_ns\":%llu,\"fsync_cpu_ns\":%llu,\"fsync_count\":%llu,\"pwrite_ns\":%llu,\"pwrite_count\":%llu,\"pread_ns\":%llu,\"pread_count\":%llu}\n",(unsigned long long)sequence,(unsigned long long)stats.start,(unsigned long long)wall,(unsigned long long)cpu,(unsigned long long)stats.sync,(unsigned long long)stats.sync_cpu,(unsigned long long)stats.syncs,(unsigned long long)stats.pwrite,(unsigned long long)stats.writes,(unsigned long long)stats.pread,(unsigned long long)stats.reads);emit(b,n);}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){const auto a=ns(CLOCK_MONOTONIC),c=ns(CLOCK_THREAD_CPUTIME_ID);const auto r=__real_fsync(fd);const int saved=errno;const auto wall=ns(CLOCK_MONOTONIC)-a,cpu=ns(CLOCK_THREAD_CPUTIME_ID)-c;stats.sync+=wall;stats.sync_cpu+=cpu;++stats.syncs;slow("fsync",fd,a,wall,cpu);errno=saved;return r;}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t size,off_t offset){const auto a=ns(CLOCK_MONOTONIC);const auto r=__real_pwrite(fd,data,size,offset);const int saved=errno;const auto wall=ns(CLOCK_MONOTONIC)-a;stats.pwrite+=wall;++stats.writes;slow("pwrite",fd,a,wall,0);errno=saved;return r;}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t size,off_t offset){const auto a=ns(CLOCK_MONOTONIC);const auto r=__real_pread(fd,data,size,offset);const int saved=errno;const auto wall=ns(CLOCK_MONOTONIC)-a;stats.pread+=wall;++stats.reads;slow("pread",fd,a,wall,0);errno=saved;return r;}
