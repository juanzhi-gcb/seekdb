#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>
static constexpr auto counts=[] {std::array<unsigned char,256> t{};for(unsigned i=0;i<256;++i)t[i]=std::popcount(i);return t;}();
extern "C" __attribute__((noinline)) unsigned count_standard(std::uint16_t x){return std::popcount(x);}
extern "C" __attribute__((noinline)) unsigned count_table(std::uint16_t x){return counts[x&255]+counts[x>>8];}
int main(){for(unsigned i=0;i<65536;++i)if(count_standard(i)!=count_table(i))return 1;
 std::mt19937 rng(821);std::vector<std::uint16_t> masks(65536);for(auto&x:masks)x=std::uint16_t(rng());
 for(unsigned round=0;round<6;++round)for(unsigned order=0;order<2;++order){bool table=(round+order)%2;auto f=table?count_table:count_standard;std::uint64_t sum=0;auto start=std::chrono::steady_clock::now();for(unsigned r=0;r<200;++r)for(auto x:masks)sum+=f(x);double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();std::cout<<(table?"table":"standard")<<" "<<round<<" "<<ms<<" "<<sum<<"\n";}
}
