#include "journal_harness.hpp"
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <random>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
#if defined(__linux__)
extern "C" int __real_fallocate(int,int,off_t,off_t);
static int mode=0,calls=0,interruptions=0;
extern "C" int __wrap_fallocate(int fd,int flags,off_t start,off_t length){++calls;
 if(mode==1){errno=EOPNOTSUPP;return -1;}if(mode==2){errno=ENOSPC;return -1;}
 if(mode==3&&interruptions++==0){errno=EINTR;return -1;}
 struct stat a{},b{};CHECK(!fstat(fd,&a));const int result=__real_fallocate(fd,flags,start,length);CHECK(!fstat(fd,&b));CHECK(a.st_size==b.st_size);return result;
}
#else
static int mode=0,calls=0,interruptions=0;
#endif
int main(){try{const auto dir=std::filesystem::path(std::getenv("MVDB_TEST_DIR")?std::getenv("MVDB_TEST_DIR"):"/tmp")/("reserve-test-"+std::to_string(getpid()));std::filesystem::create_directories(dir);
 std::mt19937 rng(929);std::string payload(2000,'\0');for(auto& c:payload)c=char(rng());
 for(mode=0;mode<4;++mode){calls=interruptions=0;const auto path=dir/("db"+std::to_string(mode));
 {JournalHarnessStore db(path);db.begin();for(int i=0;i<2048;++i)db.put_in(7,i,payload);db.commit();CHECK(db.get_in(7,173)==payload);db.begin();db.put_in(7,173,"changed");db.rollback();CHECK(db.get_in(7,173)==payload);}
 #if defined(__linux__)
 CHECK(calls>0);if(mode==1||mode==2)CHECK(calls==1);if(mode==3)CHECK(interruptions>=2);
 #endif
 {JournalHarnessStore db(path);CHECK(db.get_in(7,173)==payload);db.begin();db.put_in(7,3000,"after reopen");db.commit();}
 {JournalHarnessStore db(path);CHECK(db.get_in(7,3000)=="after reopen");CHECK(db.get_in(7,173)==payload);}
 }
 std::filesystem::remove_all(dir);std::cout<<"append reservation: unchanged length, retry, unsupported/no-space fallback, chunk crossing, rollback/reopen passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
