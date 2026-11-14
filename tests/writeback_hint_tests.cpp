#define main existing_journal_suite
#include "journal_batch_tests.cpp"
#undef main
#include <fcntl.h>
#if !defined(__linux__) || !defined(SYNC_FILE_RANGE_WRITE)
#error Native validation must exercise the Linux writeback hint
#endif
static int hint_mode=0,hint_calls=0;
extern "C" int __real_sync_file_range(int,off_t,off_t,unsigned);
extern "C" int __wrap_sync_file_range(int fd,off_t off,off_t n,unsigned flags){
 ++hint_calls;
 const int errors[]={0,ENOSYS,EOPNOTSUPP,EINVAL,EINTR,EIO,ENOSPC};
 if(hint_mode){errno=errors[hint_mode];return -1;}
 return __real_sync_file_range(fd,off,n,flags);
}
int main(){try{
 CHECK(existing_journal_suite()==0);
 std::mt19937 gen(20261115);std::vector<Store::Mutation> batch;
 for(int i=0;i<128;++i){std::string value(2048,'\0');for(auto&c:value)c=char(gen());batch.push_back({i,value});}
 const auto dir=std::filesystem::path("/tmp")/("hint-fault-"+std::to_string(getpid()));std::filesystem::create_directories(dir);
 for(auto policy:{CommitSyncPolicy::Ordered,CommitSyncPolicy::ProcessCrash})for(int mode=0;mode<7;++mode){
  const auto path=dir/(std::to_string(int(policy))+"-"+std::to_string(mode));
  {BatchStore db(path,policy);db.begin();db.put_in(7,-1,"durable");db.commit();db.begin();const auto before=rows(db);const auto save=db.savepoint();
   hint_mode=mode;hint_calls=0;bool failed=false;
   try{db.apply_batch_in(7,batch);}catch(const std::system_error&){failed=true;}
   CHECK(hint_calls>0);CHECK(failed==(mode>=5));
   if(mode>=1&&mode<=3)CHECK(hint_calls==1);
   if(failed)CHECK(rows(db)==before);
   hint_mode=0;db.restore(save);CHECK(rows(db)==before);db.rollback();
   db.begin();db.put_in(7,-2,"after fault");syncs=0;armed=true;db.commit();armed=false;
   CHECK(syncs==(policy==CommitSyncPolicy::Ordered?2:1));
  }
  {Store db(path);CHECK((rows(db)==Model{{-2,"after fault"},{-1,"durable"}}));}
 }
 std::filesystem::remove_all(dir);
 std::cout<<"hint actual call, unsupported/EINTR fallback, EIO/ENOSPC rollback, restart and unchanged sync counts passed\n";
 return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
