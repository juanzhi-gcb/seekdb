#include "../src/protocol/write_vectors.hpp"
#include <array>
#include <string>
#include <random>
#include <iostream>
int main(){try{
 std::mt19937 rng(20261114);std::size_t checks=0;
 for(unsigned n=0;n<5000;++n){
  std::array<std::string,4> text;std::string expected,actual;
  for(auto&s:text){s.resize(rng()%400);for(auto&c:s)c=char(rng());expected+=s;}
  std::array<iovec,4> parts;for(unsigned i=0;i<4;++i)parts[i]={text[i].data(),text[i].size()};
  unsigned calls=0;
  minivectordb::jsonl::detail::write_vectors(parts.data(),4,[&](const iovec*p,int count)->ssize_t{
   if(++calls%7==0){errno=EINTR;return -1;}
   std::size_t total=0;for(int i=0;i<count;++i)total+=p[i].iov_len;
   auto chosen=std::min(total,std::size_t(1+rng()%173)),left=chosen;
   for(int i=0;i<count&&left;++i){auto take=std::min(left,p[i].iov_len);actual.append(static_cast<const char*>(p[i].iov_base),take);left-=take;}
   return ssize_t(chosen);
  });if(actual!=expected)throw std::runtime_error("short write differs");++checks;
 }
 for(int error:{EPIPE,EIO}){char x='x';iovec p{&x,1};bool caught=false;try{minivectordb::jsonl::detail::write_vectors(&p,1,[&](const iovec*,int)->ssize_t{errno=error;return -1;});}catch(const std::system_error&e){caught=e.code().value()==error;}if(!caught)throw std::runtime_error("error lost");++checks;}
 {char x='x';iovec p{&x,1};bool caught=false;try{minivectordb::jsonl::detail::write_vectors(&p,1,[](const iovec*,int)->ssize_t{return 0;});}catch(const std::runtime_error&){caught=true;}if(!caught)throw std::runtime_error("zero return lost");++checks;}
 std::cout<<checks<<" writev short-write/EINTR/error checks passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
