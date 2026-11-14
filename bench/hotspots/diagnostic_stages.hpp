#pragma once
#include <array>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cerrno>
#include <utility>
namespace mvdb_diag {
inline constexpr std::array<const char*,14> labels={"json_parse","sql_parse","catalog","bind","select","topk","format","emit","execute","journal_apply","pack","row_encode","fsync","pread"};
struct Metric {std::uint64_t ns=0,calls=0;};
struct Totals {std::uint64_t requests=0,wall=0,cpu=0;std::array<Metric,14> stages{};};
inline thread_local bool active=false;
inline thread_local std::array<Metric,14> stages{};
inline thread_local unsigned kind=0,queries=0;
inline thread_local std::uint64_t start=0,cpu_start=0;
inline std::array<Totals,4> totals{};
inline std::uint64_t now(clockid_t clock=CLOCK_MONOTONIC){const int saved=errno;timespec t{};clock_gettime(clock,&t);errno=saved;return std::uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;}
struct Stage{unsigned id;std::uint64_t start;bool on;explicit Stage(unsigned i):id(i),start(active?now():0),on(active){}~Stage(){if(on){stages[id].ns+=now()-start;++stages[id].calls;}}};
template<class F> auto measure(unsigned id,F&&f){Stage s(id);return std::forward<F>(f)();}
inline void begin(){stages={};kind=0;start=now();cpu_start=now(CLOCK_THREAD_CPUTIME_ID);active=true;}
inline void classify(bool write,bool query){kind=write?1:query?(++queries<=10?2:3):0;}
inline void end(){const auto wall=now()-start,cpu=now(CLOCK_THREAD_CPUTIME_ID)-cpu_start;active=false;auto&t=totals[kind];++t.requests;t.wall+=wall;t.cpu+=cpu;for(unsigned i=0;i<labels.size();++i){t.stages[i].ns+=stages[i].ns;t.stages[i].calls+=stages[i].calls;}}
struct Report {~Report(){const char* names[]={"other","insert","query_cold","query_warm"};for(unsigned k=0;k<4;++k){const auto&t=totals[k];if(!t.requests)continue;std::fprintf(stderr,"{\"diagnostic\":\"aggregate\",\"kind\":\"%s\",\"requests\":%llu,\"wall_ns\":%llu,\"cpu_ns\":%llu,\"stages\":{",names[k],(unsigned long long)t.requests,(unsigned long long)t.wall,(unsigned long long)t.cpu);for(unsigned i=0;i<labels.size();++i)std::fprintf(stderr,"%s\"%s\":{\"ns\":%llu,\"calls\":%llu}",i?",":"",labels[i],(unsigned long long)t.stages[i].ns,(unsigned long long)t.stages[i].calls);std::fprintf(stderr,"}}\n");}}};
}
