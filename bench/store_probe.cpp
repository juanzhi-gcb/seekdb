#include "minivectordb/store.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace minivectordb;
int main(){const auto path=std::filesystem::path("/tmp")/("cached-read-"+std::to_string(getpid()));
 {Store db(path,0,true,0,65536,CommitSyncPolicy::ProcessCrash,true,true,true,false,true,true,true);
 db.begin();for(int i=0;i<8;++i)db.put_in(1,i,"catalog-schema-field-"+std::to_string(i));db.commit();
 std::uint64_t sum=0;for(int i=0;i<100;++i)sum+=db.get_in(1,i%8)->size();
 for(unsigned round=0;round<5;++round){auto start=std::chrono::steady_clock::now();for(unsigned i=0;i<200000;++i){auto value=db.get_in(1,i%8);if(!value||*value!="catalog-schema-field-"+std::to_string(i%8))return 1;sum+=value->size();}auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();std::cout<<round<<" "<<ms<<" "<<sum<<"\n";}}
 std::filesystem::remove(path);}
