#pragma once
#include "minivectordb/database.hpp"
#include "minivectordb/store.hpp"
#include <cstdlib>
#include <string_view>
// Reuse the unchanged SQL/fault oracles against an explicitly selected backend.
class JournalHarnessDatabase:public minivectordb::Database {
  static minivectordb::Options select(minivectordb::Options o){
    o.journal_store=true;
    if(const char* mode=std::getenv("MVDB_TEST_COMMIT_SYNC")){
      const std::string_view value(mode);
      if(value=="fdatasync")o.commit_sync_policy=minivectordb::CommitSyncPolicy::ProcessCrashDataSync;
      else if(value=="nosync")o.commit_sync_policy=minivectordb::CommitSyncPolicy::ProcessCrashNoSync;
    }
    return o;
  }
public:
  explicit JournalHarnessDatabase(const std::filesystem::path& p,minivectordb::Options o={}):Database(p,select(std::move(o))){}
};
class JournalHarnessStore:public minivectordb::Store {
public:
  template<class... Args> explicit JournalHarnessStore(const std::filesystem::path& p,Args&&...):Store(p,0,true,0,65536,minivectordb::CommitSyncPolicy::Ordered,true,true,true,false,true,true,true){}
};
