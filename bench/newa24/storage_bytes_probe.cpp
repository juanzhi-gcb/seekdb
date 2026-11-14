#include "journal_harness.hpp"
#include <random>
#include <iostream>
int main(int argc,char**argv){try{
  if(argc!=2)return 2;std::filesystem::path path=argv[1];std::filesystem::remove(path);std::mt19937 rng(20261114);
  {
    JournalHarnessStore db(path);
    for(int batch=0;batch<100;++batch){db.begin();std::vector<minivectordb::Store::Mutation> changes;
      for(int i=0;i<100;++i){std::string text(64+rng()%1800,'\0');for(auto&c:text)c=char(rng()%5?0:rng());changes.push_back({batch*100+i,text});}
      db.apply_batch_in(7,changes);db.commit();
    }
    for(int batch=0;batch<30;++batch){db.begin();auto point=db.savepoint();std::vector<minivectordb::Store::Mutation> changes;
      for(int i=0;i<100;++i){const auto key=std::int64_t((batch*317+i*37)%10000);changes.push_back({key,i%3?std::optional<std::string>(std::string(80+unsigned(i),'x')):std::nullopt});}
      db.apply_batch_in(7,changes);if(batch%4==0)db.restore(point);db.put_in(9,-batch-1,"other-space");db.commit();
    }
  }
  {JournalHarnessStore db(path);db.begin();db.put_in(7,INT64_MIN,"min");db.put_in(7,INT64_MAX,"max");db.commit();std::size_t n=0;db.scan_in(7,[&](auto,auto){++n;});std::cout<<"rows="<<n<<" file_bytes="<<std::filesystem::file_size(path)<<'\n';}
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
