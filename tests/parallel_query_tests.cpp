#include "minivectordb/database.hpp"
#include "minivectordb/profile.hpp"
#include "../src/sql/query_worker.hpp"
#include <atomic>
#include <bit>
#include <cfenv>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
struct Result {std::vector<Row> rows;ExecutionInfo info;};
Result query(Database& db,const std::string& text){Result r;r.info=db.execute(text,{{},[&](const Row& row){r.rows.push_back(row);}});return r;}
void same(const Result& a,const Result& b){CHECK(a.rows==b.rows);for(std::size_t i=0;i<a.rows.size();++i)for(std::size_t c=0;c<a.rows[i].size();++c)if(std::holds_alternative<double>(a.rows[i][c]))CHECK(std::bit_cast<std::uint64_t>(std::get<double>(a.rows[i][c]))==std::bit_cast<std::uint64_t>(std::get<double>(b.rows[i][c])));}
std::string vector(unsigned seed){std::string s="[";for(unsigned d=0;d<128;++d){if(d)s+=',';s+=std::to_string(int((seed*37+d*19)%101)-50);}return s+"]";}
int main(){try{
  {
    sql::QueryWorker worker;std::atomic<unsigned> completed=0;
    for(unsigned i=0;i<1000;++i){unsigned a=0,b=0;worker.run([&]{a=i+1;},[&]{b=i+2;++completed;});CHECK(a==i+1&&b==i+2&&completed==i+1);}
    for(unsigned failure=0;failure<3;++failure){bool threw=false;std::atomic<bool> second_done=false;try{worker.run([&]{if(failure!=1)throw std::runtime_error("first");},[&]{second_done=true;if(failure!=0)throw std::runtime_error("second");});}catch(const std::runtime_error&){threw=true;}CHECK(threw&&second_done);}
    worker.run([]{},[]{}); // Exceptions do not poison later requests.
  }
  const auto root=std::filesystem::path(std::getenv("MVDB_TEST_DIR")?std::getenv("MVDB_TEST_DIR"):"/tmp")/("parallel-"+std::to_string(getpid()));std::filesystem::create_directories(root);
  Options on,off;off.parallel_exact_scan=false;unsigned parallel_queries=0;
  {
    Database a(root/"on.db",on),b(root/"off.db",off);
    auto execute=[&](const std::string& sql){CHECK(a.execute(sql).affected_rows==b.execute(sql).affected_rows);};
    execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(128),g BIGINT,body VARCHAR(20))");
    for(int start=0;start<10003;start+=100){std::string sql="INSERT INTO t VALUES";for(int i=start;i<std::min(10003,start+100);++i){if(i!=start)sql+=',';sql+="("+std::to_string(i-5001)+",'"+vector(unsigned(i)%101)+"',"+std::to_string(i%10)+",'r"+std::to_string(i)+"')";}execute(sql);}
    auto verify=[&](){for(unsigned phase=0;phase<5;++phase)for(unsigned k:{1U,10U,64U})for(const std::string filter:{""," WHERE g=4"}){const auto sql="SELECT id,body,L2_DISTANCE(v,'"+vector(phase*13)+"') AS d FROM t"+filter+" ORDER BY d LIMIT "+std::to_string(k);const auto x=query(a,sql),y=query(b,sql);same(x,y);if(x.info.parallel_query_workers){CHECK(filter.empty());CHECK(x.info.parallel_query_workers==2);++parallel_queries;}}};
    verify();execute("CREATE VECTOR INDEX vi ON t(v)");verify();
    execute("BEGIN");execute("UPDATE t SET body='changed' WHERE g=4");verify();execute("ROLLBACK");verify();
    profile::set_enabled(true);auto profiled=query(a,"SELECT id,L2_DISTANCE(v,'"+vector(0)+"') AS d FROM t ORDER BY d LIMIT 10");CHECK(profiled.info.parallel_query_workers==0);profile::set_enabled(false);
    const auto original=std::fegetround();CHECK(std::fesetround(FE_DOWNWARD)==0);const auto sql="SELECT id,L2_DISTANCE(v,'"+vector(17)+"') AS d FROM t ORDER BY d LIMIT 10";auto directed=query(a,sql);same(directed,query(b,sql));CHECK(directed.info.parallel_query_workers==0);CHECK(std::fesetround(original)==0);
    verify();
  }
#if defined(__x86_64__) && defined(__GNUC__)
  if(__builtin_cpu_supports("avx2")&&std::thread::hardware_concurrency()>1)CHECK(parallel_queries>0);
#endif
  std::filesystem::remove_all(root);std::cout<<"{\"parallel_queries\":"<<parallel_queries<<",\"worker_tasks\":1004,\"exact_bits\":true}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
