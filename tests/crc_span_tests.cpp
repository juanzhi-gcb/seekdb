#include "../src/storage/page_checksum.hpp"
#include <iostream>
#include <random>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
using namespace minivectordb::detail;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
std::uint32_t reference(std::uint32_t crc,const unsigned char*p,std::size_t n){while(n--)crc=(crc>>8)^page_crc32_table[(crc^*p++)&255];return crc;}
int main(){try{
 std::mt19937 rng(9152026);std::vector<unsigned char> data(131200);for(auto&x:data)x=static_cast<unsigned char>(rng());std::uint64_t cases=0;
 CHECK(~page_crc32_span(0xffffffffU,reinterpret_cast<const unsigned char*>("123456789"),9)==0xcbf43926U);
 for(std::size_t offset=0;offset<32;++offset)for(std::size_t n=0;n<=4096;++n){auto seed=std::uint32_t(rng());const auto a=reference(seed,data.data()+offset,n);CHECK(page_crc32_span(seed,data.data()+offset,n)==a);CHECK(page_crc32_span_slicing(seed,data.data()+offset,n)==a);++cases;}
 for(unsigned i=0;i<5000;++i){const auto n=std::size_t(rng()%65537),offset=std::size_t(rng()%64);const auto seed=std::uint32_t(rng());const auto a=reference(seed,data.data()+offset,n);CHECK(page_crc32_span(seed,data.data()+offset,n)==a);const auto split=std::size_t(rng()%(n+1));CHECK(page_crc32_span(page_crc32_span(seed,data.data()+offset,split),data.data()+offset+split,n-split)==a);++cases;}
 const auto page=std::size_t(sysconf(_SC_PAGESIZE));auto* memory=static_cast<unsigned char*>(mmap(nullptr,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));CHECK(memory!=MAP_FAILED);CHECK(mprotect(memory+page,page,PROT_READ|PROT_WRITE)==0);for(std::size_t i=0;i<page;++i)memory[page+i]=static_cast<unsigned char>(rng());
 for(std::size_t n=0;n<=4096 && n<=page;++n){const auto* start=memory+2*page-n;const auto seed=std::uint32_t(rng());CHECK(page_crc32_span(seed,start,n)==reference(seed,start,n));++cases;}
 CHECK(munmap(memory,page*3)==0);
 std::cout<<"CRC streaming/unaligned/guard-page checks: "<<cases<<", PCLMUL active="<<page_crc32_pclmul_available()<<'\n';
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
