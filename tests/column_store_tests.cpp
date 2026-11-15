#include "minivectordb/database.hpp"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
struct Result {std::vector<Row> rows;ExecutionInfo info;};
std::string literal(const std::vector<float>& values) {
  std::ostringstream out;out<<std::setprecision(9)<<'[';
  for(std::size_t i=0;i<values.size();++i){if(i)out<<',';out<<values[i];}
  out<<']';return out.str();
}
Result run(Database& db,const std::string& sql) {
  Result result;result.info=db.execute(sql,{{},[&](const Row& row){result.rows.push_back(row);}});return result;
}
void compare(const Result& fast,const Result& reference) {
  CHECK(fast.rows.size()==reference.rows.size());
  for(std::size_t i=0;i<fast.rows.size();++i) {
    CHECK(fast.rows[i].size()==reference.rows[i].size());
    CHECK(std::get<std::int64_t>(fast.rows[i][0])==std::get<std::int64_t>(reference.rows[i][0]));
    CHECK(std::abs(std::get<double>(fast.rows[i][1])-std::get<double>(reference.rows[i][1]))<1e-12);
    for(std::size_t c=2;c<fast.rows[i].size();++c)CHECK(fast.rows[i][c]==reference.rows[i][c]);
  }
}
std::vector<Row> query(Database& db,const std::string& sql) {return run(db,sql).rows;}
void shapes(const std::filesystem::path& root) {
  const auto fast_path=root/"fast.db",reference_path=root/"reference.db";
  Options fast,reference;fast.column_store=true;reference.column_store=false;
  const std::string q=literal({1,2,-1,0});
  const std::string base="SELECT id,L2_DISTANCE(v,'"+q+"') AS d,g,body FROM t";
  const std::vector<std::string> queries{
    base+" ORDER BY d LIMIT 7;",
    base+" WHERE g=2 ORDER BY d LIMIT 7;",
    base+" WHERE g=99 ORDER BY d LIMIT 7;",
    base+" WHERE id=14 ORDER BY d LIMIT 7;",
    "SELECT id,L2_DISTANCE(v,'"+q+"') AS d FROM t WHERE g=3 ORDER BY d LIMIT 7;",
    "SELECT id,L2_DISTANCE(v,'"+q+"') AS d,v FROM t ORDER BY d LIMIT 7;",
    "SELECT id,L2_DISTANCE(v,'"+q+"') AS d,L2_DISTANCE(v,'[0,0,0,0]') AS e FROM t ORDER BY d LIMIT 7;",
    base+" ORDER BY d DESC LIMIT 7;",
    base+" WHERE g<2 ORDER BY d LIMIT 7;",
    base+" ORDER BY d LIMIT 0;"
  };
  auto check_all=[&](Database& a,Database& b) {for(const auto& sql:queries)compare(run(a,sql),run(b,sql));};
  {
    Database a(fast_path,fast),b(reference_path,reference);
    const std::string schema="CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(4),g BIGINT,body VARCHAR(24));";
    CHECK(a.execute(schema).affected_rows==0);CHECK(b.execute(schema).affected_rows==0);
    std::string insert="INSERT INTO t VALUES ";
    for(int i=0;i<80;++i) {
      if(i)insert+=',';
      const std::string body="body-"+std::to_string(i);
      insert+='('+std::to_string(i*3-100)+",'["+std::to_string(i%7)+","+std::to_string(i%3)+",-1,2]',"+
        std::to_string(i%5)+",'"+body+"')";
    }
    insert+=';';
    CHECK(a.execute(insert).affected_rows==80);CHECK(b.execute(insert).affected_rows==80);
    check_all(a,b);
    // A non-SELECT invalidates the cache; the next query must rebuild identically.
    CHECK(a.execute("UPDATE t SET g=4,body='updated' WHERE id=14").affected_rows==1);
    CHECK(b.execute("UPDATE t SET g=4,body='updated' WHERE id=14").affected_rows==1);
    check_all(a,b);
    CHECK(a.execute("BEGIN").affected_rows==0);CHECK(b.execute("BEGIN").affected_rows==0);
    CHECK(a.execute("DELETE FROM t WHERE g=0").affected_rows>0);
    CHECK(b.execute("DELETE FROM t WHERE g=0").affected_rows>0);
    check_all(a,b);
    CHECK(a.execute("ROLLBACK").affected_rows==0);CHECK(b.execute("ROLLBACK").affected_rows==0);
    check_all(a,b);
  }
  {
    // Reopen: the cache is process-local and must rebuild from the durable store.
    Database a(fast_path,fast),b(reference_path,reference);
    check_all(a,b);
  }
}
void varchar_filter(const std::filesystem::path& root) {
  Options fast,reference;fast.column_store=true;reference.column_store=false;
  Database a(root/"vfast.db",fast),b(root/"vreference.db",reference);
  CHECK(a.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(4),g VARCHAR(16),body VARCHAR(24));").affected_rows==0);
  CHECK(b.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(4),g VARCHAR(16),body VARCHAR(24));").affected_rows==0);
  std::string insert="INSERT INTO t VALUES ";
  const std::string labels[]{"a","b","中","😀"};
  for(int i=0;i<40;++i) {
    if(i)insert+=',';
    insert+='('+std::to_string(i)+",'["+std::to_string(i%5)+",1,-2,3]','"+labels[i%4]+"','v"+std::to_string(i)+"')";
  }
  insert+=';';
  CHECK(a.execute(insert).affected_rows==40);CHECK(b.execute(insert).affected_rows==40);
  const std::string q=literal({0,1,2,3});
  const std::vector<std::string> queries{
    "SELECT id,L2_DISTANCE(v,'"+q+"') AS d,g,body FROM t ORDER BY d LIMIT 6;",
    "SELECT id,L2_DISTANCE(v,'"+q+"') AS d,g,body FROM t WHERE g='中' ORDER BY d LIMIT 6;",
    "SELECT id,L2_DISTANCE(v,'"+q+"') AS d,g,body FROM t WHERE g='absent' ORDER BY d LIMIT 6;",
    "SELECT id,L2_DISTANCE(v,'"+q+"') AS d,body FROM t WHERE g='😀' ORDER BY d LIMIT 6;"
  };
  for(const auto& sql:queries)compare(run(a,sql),run(b,sql));
}
void budget_and_ties(const std::filesystem::path& root) {
  Options fast,reference;fast.column_store=true;reference.column_store=false;
  fast.scan_snapshot_bytes=1024;reference.scan_snapshot_bytes=1024;
  Database a(root/"bfast.db",fast),b(root/"breference.db",reference);
  CHECK(a.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(4),g BIGINT,body VARCHAR(24));").affected_rows==0);
  CHECK(b.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(4),g BIGINT,body VARCHAR(24));").affected_rows==0);
  std::string insert="INSERT INTO t VALUES ";
  for(int i=0;i<20;++i) {
    if(i)insert+=',';
    insert+='('+std::to_string(19-i)+",'[1,1,1,1]',1,'t"+std::to_string(i)+"')";
  }
  insert+=';';
  CHECK(a.execute(insert).affected_rows==20);CHECK(b.execute(insert).affected_rows==20);
  const auto sql="SELECT id,L2_DISTANCE(v,'[0,0,0,0]') AS d,g,body FROM t ORDER BY d LIMIT 5;";
  compare(run(a,sql),run(b,sql));
}
int main() {
  try {
    const auto root=std::filesystem::path(std::getenv("MVDB_TEST_DIR")?std::getenv("MVDB_TEST_DIR"):"/tmp")/("column-store-"+std::to_string(getpid()));
    std::filesystem::remove_all(root);std::filesystem::create_directories(root);
    shapes(root);varchar_filter(root);budget_and_ties(root);
    std::filesystem::remove_all(root);
    std::cout<<"PASS column store differential shapes, filters, ties, mutations, rollback, reopen, budget fallback\n";
    return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}