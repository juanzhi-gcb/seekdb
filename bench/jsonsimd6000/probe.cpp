#include "protocol/jsonl.hpp"
#include <chrono>
#include <iostream>
using namespace minivectordb;
int main(){for(int n:{1024,131072,1048576})for(int form=0;form<3;++form){
 std::string sql(std::size_t(n),'x');if(form==1)for(int at=1023;at<n;at+=1024)sql[std::size_t(at)]='\n';
 if(form==2)for(int at=0;at+8<n;at+=16)sql.replace(std::size_t(at),6,"中文");
 const auto frame="{\"id\":123,\"op\":\"execute\",\"sql\":"+jsonl::quote(sql)+"}";const int repeats=50000000/n;
 for(int round=0;round<3;++round){std::size_t sum=0;auto start=std::chrono::steady_clock::now();for(int i=0;i<repeats;++i){auto p=jsonl::Parser(frame).parse();sum+=p.at("sql").text.size();}const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();std::cout<<"{\"bytes\":"<<n<<",\"form\":"<<form<<",\"round\":"<<round<<",\"ms\":"<<ms<<",\"sum\":"<<sum<<"}\n";}
}}
