#pragma once
#include <array>
#include <cstdint>
#include <ctime>
#include <utility>
namespace mvdb_diag {
inline thread_local std::array<std::uint64_t,7> stages{};
inline std::uint64_t now(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return std::uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;}
struct Stage {unsigned id;std::uint64_t start;explicit Stage(unsigned i):id(i),start(now()){}~Stage(){stages[id]+=now()-start;}};
template<class F> auto measure(unsigned id,F&& f){Stage s(id);return std::forward<F>(f)();}
}
