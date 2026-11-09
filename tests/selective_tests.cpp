#include "minivectordb/database.hpp"
#include "minivectordb/row_codec.hpp"
#include "minivectordb/store.hpp"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__)); } while(false)
struct Result { std::vector<Row> rows; ExecutionInfo info; };
Result query(Database& db,const std::string& text) {
  Result r; r.info=db.execute(text,{{},[&](const Row& row){r.rows.push_back(row);}}); return r;
}
void comparisons(const std::filesystem::path& p) {
  {
    Database db(p);
    db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2),s VARCHAR(20),n BIGINT)");
    db.execute("INSERT INTO t VALUES(-3,[3,4],'中😀',0),(1,[0,1],'a',1),(2,[0,-1],'b',1),(9,[8,9],'z',0)");
    std::string sql="INSERT INTO t VALUES(10,[1,2],'a"; sql.push_back('\0'); sql+="b',1)"; db.execute(sql);
  }
  std::vector<std::string> statements={
    "SELECT * FROM t", "SELECT id FROM t WHERE n=1", "SELECT id FROM t WHERE id=1",
    "SELECT * FROM t WHERE id=999", "SELECT * FROM t LIMIT 0",
    "SELECT id,s,L2_DISTANCE(v,[0,0]) AS d FROM t WHERE n=1 ORDER BY d LIMIT 2",
    "SELECT id,L2_DISTANCE(v,[0,0]),L2_DISTANCE(v,[1,1]) FROM t WHERE n=1 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 2",
    "SELECT * FROM t ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 2",
    "SELECT id FROM t WHERE n=1 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 999",
    "SELECT id,L2_DISTANCE(v,[0,0]) FROM t ORDER BY n DESC,L2_DISTANCE(v,[0,0])",
    "SELECT s FROM t ORDER BY s DESC", "SELECT id FROM t WHERE n=9 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 2"};
  std::vector<Result> eager;
  { Database db(p,{p.parent_path()/"sort",4096,false}); for(const auto& sql:statements)eager.push_back(query(db,sql)); }
  {
    Database db(p,{p.parent_path()/"sort",4096,true});
    for(std::size_t i=0;i<statements.size();++i) {
      auto r=query(db,statements[i]); CHECK(r.rows==eager[i].rows);
      CHECK(r.info.rows_examined==eager[i].info.rows_examined);
      CHECK(r.info.distances_computed==eager[i].info.distances_computed);
    }
    auto filtered=query(db,"SELECT id,L2_DISTANCE(v,[0,0]) FROM t WHERE n=1 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 2");
    CHECK(filtered.info.vector_values_materialized==3);
    CHECK(filtered.info.distances_computed==3);
    CHECK(query(db,"SELECT id FROM t WHERE n=1").info.vector_values_materialized==0);
    CHECK(query(db,"SELECT id FROM t LIMIT 0").info.cells_materialized==0);
    CHECK(query(db,"SELECT * FROM t WHERE id=1").info.cells_materialized==4);
    CHECK(query(db,"SELECT id,L2_DISTANCE(v,[0,0]),L2_DISTANCE(v,[1,1]) FROM t WHERE n=1 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 2").info.vector_values_materialized==5);
    db.execute("BEGIN"); db.execute("UPDATE t SET n=1 WHERE id=9");
    CHECK(query(db,"SELECT id,L2_DISTANCE(v,[0,0]) FROM t WHERE n=1").info.vector_values_materialized==4);
    db.execute("ROLLBACK");
    CHECK(query(db,"SELECT id FROM t WHERE n=1").rows.size()==3);
  }
  { Database db(p,{p.parent_path()/"sort",4096,false}); CHECK(query(db,"SELECT id FROM t WHERE n=1").info.vector_values_materialized==5); }
}
void corrupt_rows(const std::filesystem::path& dir) {
  Schema s{1,"t",{{"id",ColumnType::Bigint,0},{"v",ColumnType::Vector,2},{"s",ColumnType::Varchar,20},{"n",ColumnType::Bigint,0}},0};
  auto good=encode_row(s,{std::int64_t(1),std::vector<float>{1,2},std::string("ok"),std::int64_t(0)});
  std::vector<std::string> bad;
  auto nonfinite=good; nonfinite[19]=char(0x80); nonfinite[20]=char(0x7f); bad.push_back(nonfinite);
  auto unicode=good; unicode[29]=char(0x80); bad.push_back(unicode);
  auto wrongkey=good; wrongkey[5]=2; bad.push_back(wrongkey);
  bad.push_back(good+"x");
  for(std::size_t n=0;n<good.size();++n)bad.push_back(good.substr(0,n));
  int index=0;
  for(const auto& bytes:bad) {
    auto path=dir/("bad-"+std::to_string(index++));
    { Database db(path); db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2),s VARCHAR(20),n BIGINT)"); }
    { Store store(path); store.begin(); store.put_in(1,1,bytes); store.commit(); }
    for(bool selective:{false,true}) {
      Database db(path,{dir/"sort",4096,selective});
      for(const auto* sql:{"SELECT id FROM t WHERE n=1","SELECT id FROM t WHERE id=1","SELECT id FROM t ORDER BY n"}) {
        bool rejected=false; try { db.execute(sql); } catch(const Error& e) { rejected=e.code()==ErrorCode::Corruption; }
        CHECK(rejected);
      }
    }
  }
}
int main() {
  try {
    const auto* root=std::getenv("MVDB_TEST_DIR"); CHECK(root);
    std::filesystem::create_directories(root); auto name=(std::filesystem::path(root)/"selective-XXXXXX").string(); CHECK(mkdtemp(name.data()));
    std::filesystem::path dir(name); comparisons(dir/"db"); corrupt_rows(dir); std::filesystem::remove_all(dir);
    std::cout<<"PASS selective\n";
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
