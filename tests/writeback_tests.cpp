#include "journal_harness.hpp"
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <random>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
static int mode=0,calls=0,interruptions=0;
#if defined(__linux__)
extern "C" int __real_sync_file_range(int,off_t,off_t,unsigned);
extern "C" int __wrap_sync_file_range(int fd,off_t start,off_t length,unsigned flags){++calls;
 CHECK(start>=8192 && start%4096==0 && length>0 && length%4096==0);CHECK(flags==SYNC_FILE_RANGE_WRITE);
 struct stat st{};CHECK(!fstat(fd,&st));CHECK(start+length<=st.st_size);
 if(mode==1){errno=EOPNOTSUPP;return -1;}if(mode==2){errno=EIO;return -1;}
 if(mode==3&&interruptions++==0){errno=EINTR;return -1;}
 return __real_sync_file_range(fd,start,length,flags);
}
#endif
int main(){try{const auto dir=std::filesystem::path(std::getenv("MVDB_TEST_DIR")?std::getenv("MVDB_TEST_DIR"):"/tmp")/("writeback-test-"+std::to_string(getpid()));std::filesystem::create_directories(dir);
 std::mt19937 rng(929);std::string payload(2000,'\0');for(auto& c:payload)c=char(rng());
 for(mode=0;mode<4;++mode){calls=interruptions=0;const auto path=dir/("db"+std::to_string(mode));
 {JournalHarnessStore db(path);db.begin();db.put_in(7,9000,"baseline");db.commit();db.begin();
  std::vector<Store::Mutation> changes;for(int i=0;i<100;++i)changes.push_back({i,payload});
  bool failed=false;try{db.apply_batch_in(7,changes);db.commit();}catch(const std::system_error&){failed=true;db.rollback();}
#if defined(__linux__)
  CHECK(failed==(mode==2));CHECK(calls>0);if(mode==1)CHECK(calls==1);if(mode==3)CHECK(interruptions>=2);
#else
  CHECK(!failed);
#endif
  CHECK(db.get_in(7,9000)=="baseline");if(!failed)CHECK(db.get_in(7,73)==payload);else CHECK(!db.get_in(7,73));
 }
 {JournalHarnessStore db(path);CHECK(db.get_in(7,9000)=="baseline");
#if defined(__linux__)
  if(mode==2)CHECK(!db.get_in(7,73));else
#endif
  CHECK(db.get_in(7,73)==payload);
  db.begin();db.put_in(7,3000,"after reopen");db.commit();}
 {JournalHarnessStore db(path);CHECK(db.get_in(7,3000)=="after reopen");}
 }
 std::filesystem::remove_all(dir);std::cout<<"writeback: page bounds, no WAIT flags, retry, unsupported fallback, I/O rollback/reopen passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
