#include "journal_harness.hpp"
#include <map>
#include <random>
#include <iostream>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
int main(){try{
 const auto path=std::filesystem::path(std::getenv("MVDB_TEST_DIR")?std::getenv("MVDB_TEST_DIR"):"/tmp")/("stream-cache-"+std::to_string(getpid()));std::filesystem::create_directories(path);std::map<std::int64_t,std::string> expected;
 {
 JournalHarnessStore db(path/"db");std::mt19937 rng(20261113);std::vector<int> keys;for(int i=0;i<4096;++i)keys.push_back(i-2048);std::shuffle(keys.begin(),keys.end(),rng);db.begin();
 for(auto k:keys){std::string value(256,'\0');for(auto& c:value)c=char(rng());expected[k]=value;db.put_in(7,k,value);db.put_in(9,k,"index:"+std::to_string(k));}db.commit();
 for(bool retain:{false,true}){std::map<std::int64_t,std::string> actual;db.scan_cache_in(7,[&](auto k,auto v){CHECK(actual.emplace(k,v).second);return retain;});CHECK(actual==expected);
 actual.clear();db.scan_cache_equal_in(9,7,[&](auto k,auto a,auto b){CHECK(a=="index:"+std::to_string(k));CHECK(actual.emplace(k,b).second);return retain;});CHECK(actual==expected);}
 db.begin();const auto root=db.savepoint();bool mutate=true;std::map<std::int64_t,std::string> actual;
 db.scan_cache_equal_in(9,7,[&](auto k,auto a,auto b){CHECK(a=="index:"+std::to_string(k));actual.emplace(k,b);if(mutate){mutate=false;db.erase_in(7,0);db.put_in(7,9000,"new");db.put_in(9,9000,"new index");}return true;});CHECK(actual==expected);db.restore(root);db.rollback();
 db.begin();db.erase_in(9,0);unsigned calls=0;bool mismatch=false;try{db.scan_cache_equal_in(9,7,[&](auto,auto,auto){++calls;return true;});}catch(const Store::KeySetMismatch&){mismatch=true;}CHECK(mismatch&&calls==0);db.rollback();
 bool threw=false;try{db.scan_cache_in(7,[](auto,auto)->bool{throw std::runtime_error("visitor failure");});}catch(...){threw=true;}CHECK(threw);
 }
 {JournalHarnessStore db(path/"db");std::map<std::int64_t,std::string> actual;db.scan_in(7,[&](auto k,auto v){actual.emplace(k,v);});CHECK(actual==expected);}
 std::filesystem::remove_all(path);std::cout<<"physical cache scans: 4096 random rows, paired key proof, callback mutation, rollback, decline, exception, reopen passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
