#include "minivectordb/database.hpp"
#include "../src/vector/validated.hpp"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
int main(){try{
  auto root=std::filesystem::path(std::getenv("MVDB_TEST_DIR")?std::getenv("MVDB_TEST_DIR"):"/tmp")/("fixedsql-"+std::to_string(getpid()));
  std::filesystem::create_directories(root);Options on,off;off.fixed_point_scan_cache=false;
  auto vec=[](int seed,bool nongrid=false){std::string s="'[";for(int d=0;d<32;++d){if(d)s+=",";s+=nongrid&&d==0?"0.10000000149011612":std::to_string(((seed+d*13)%97-48)*0.125);}return s+"]'";};
  {
    Database a(root/"on.db",on),b(root/"off.db",off);
    auto execute=[&](std::string s){CHECK(a.execute(s).affected_rows==b.execute(s).affected_rows);};
    execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(32),g BIGINT,body VARCHAR(64))");
    for(int i=0;i<83;++i)execute("INSERT INTO t VALUES("+std::to_string(i-40)+","+vec(i%19)+","+std::to_string(i%3)+",'data')");
    std::uint64_t used=0;
    auto verify=[&](){for(int trial=0;trial<8;++trial)for(auto filter:{""," WHERE g=1"," WHERE g=99"})for(int k:{0,1,10,100}){
      const auto s="SELECT id,body,L2_DISTANCE(v,"+vec(trial,trial==7)+") AS d FROM t"+filter+" ORDER BY d LIMIT "+std::to_string(k);
      std::vector<Row>x,y;auto info=a.execute(s,{{},[&](const Row&r){x.push_back(r);}});b.execute(s,{{},[&](const Row&r){y.push_back(r);}});CHECK(x==y);used+=info.fixed_point_rows;
      if(trial==7)CHECK(info.fixed_point_rows==0);
    }};
    verify();execute("CREATE VECTOR INDEX vi ON t(v)");verify();
    execute("BEGIN");execute("UPDATE t SET v="+vec(91)+" WHERE g=1");verify();execute("ROLLBACK");verify();
    execute("DELETE FROM t WHERE g=2");verify();
    if(detail::has_avx2())CHECK(used>0);
    CHECK(a.execute("SELECT id,L2_DISTANCE(v,"+vec(0)+") AS d FROM t ORDER BY d LIMIT 10").typed_cache_bytes<=on.scan_snapshot_bytes);
  }
  {Database a(root/"on.db",on),b(root/"off.db",off);std::vector<Row>x,y;a.execute("SELECT * FROM t",{{},[&](const Row&r){x.push_back(r);}});b.execute("SELECT * FROM t",{{},[&](const Row&r){y.push_back(r);}});CHECK(x==y);}
  std::filesystem::remove_all(root);std::cout<<"fixed16 SQL ties, filters, index, rollback, fallback, reopen passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
