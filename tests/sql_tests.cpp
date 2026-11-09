#include "minivectordb/database.hpp"
#include "minivectordb/row_codec.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <sys/wait.h>
#include <sys/resource.h>
#include <csignal>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
template<class F> void rejects(F f) {
  bool failed=false;
  try {
    f();
  }
  catch(const std::exception&) {
    failed=true;
  }
  CHECK(failed);
}
struct Result {
  std::vector<std::string> columns;
  std::vector<Row> rows;
  ExecutionInfo info;
};
Result q(Database&d,std::string_view sql) {
  Result r;
  r.info=d.execute(sql, {
    [&](const auto& c) {
      r.columns=c;
    },[&](const Row& row) {
      r.rows.push_back(row);
    }
  });
  return r;
}
std::int64_t id(const Result&r,std::size_t n=0) {
  return std::get<std::int64_t>(r.rows.at(n).at(0));
}
void codec() {
  Schema s {
    1,"t", {
      {
        "id",ColumnType::Bigint,0
      }, {
        "name",ColumnType::Varchar,3
      }, {
        "v",ColumnType::Vector,2
      }
    },0
  };
  Row r {
    INT64_MIN,std::string("a\0b",3),std::vector<float> {
      1.25f,-2
    }
  };
  auto bytes=encode_row(s,r);
  CHECK(decode_row(s,bytes)==r);
  for(std::size_t n=0;n<bytes.size();++n)rejects([&] {
    decode_row(s,std::string_view(bytes).substr(0,n));
  });
  rejects([&] {
    decode_row(s,bytes+"x");
  });
  auto bad=bytes;
  for(std::size_t n=13;n<17&&n<bad.size();++n)bad[n]=char(255);
  rejects([&] {
    decode_row(s,bad);
  });
  r[1]=std::string("中😀");
  CHECK(decode_row(s,encode_row(s,r))==r);
  r[1]=std::string("abcd");
  rejects([&] {
    encode_row(s,r);
  });
  r[1]=std::string("ok");
  r[2]=std::vector<float> {
    INFINITY,1
  };
  rejects([&] {
    encode_row(s,r);
  });
}
void sql(const std::filesystem::path&p) {
  {
    Database d(p, {
      p.parent_path()/"sort",4096
    });
    d.execute("/* hello */ CREATE TABLE T (id BIGINT PRIMARY KEY, name VARCHAR(4), v VECTOR(2), score BIGINT); --tail");
    d.execute("CREATE TABLE u (id BIGINT, s VARCHAR(2), PRIMARY KEY(id))");
    CHECK(d.execute("INSERT INTO t VALUES (1,'one',[3,4],10),(2,'two','[0,1]',20),(3,'三',[0,-1],20)").affected_rows==3);
    d.execute("INSERT INTO u(s,id) VALUES ('好',1)");
    CHECK(id(q(d,"SELECT id FROM u"))==1);
    auto all=q(d,"select name,id from T where score>=20 AND id<3");
    CHECK(all.rows.size()==1&&std::get<std::string>(all.rows[0][0])=="two");
    auto near=q(d,"SELECT id,L2_DISTANCE(v,[0,0]) AS distance FROM t WHERE score>=20 ORDER BY distance LIMIT 10");
    CHECK(near.rows.size()==2&&id(near)==2&&id(near,1)==3);
    CHECK(std::get<double>(near.rows[0][1])==1);
    CHECK(near.info.distances_computed==2);
    CHECK(q(d,"select * from t limit 0").rows.empty());
    CHECK(q(d,"select id from t where id=9223372036854775807").rows.empty());
    CHECK(q(d,"select id from t where id=2").info.rows_examined==1);
    CHECK(id(q(d,"select id from t order by score desc, name desc limit 1"))==3);
    for(auto bad: {
      "SELECT * FROM t OR id=1","SELECT NULL FROM t","SELECT count(*) FROM t","SELECT * FROM t; SELECT * FROM u","UPDATE t SET id=9","INSERT INTO t VALUES (4,'abcde',[1,2],1)","INSERT INTO t VALUES (4,'four',[1e100,2],1)","INSERT INTO t VALUES (4,'four',[1],1)","CREATE TABLE bad(id BIGINT)","CREATE TABLE bad(id BIGINT PRIMARY KEY,id BIGINT)","CREATE TABLE bad(id BIGINT PRIMARY KEY,v VECTOR(2),w VECTOR(2))","COMMIT","ROLLBACK"
    })rejects([&] {
      d.execute(bad);
    });
    CHECK(d.execute("UPDATE t SET name='O''K',v=[0,0] WHERE id=1").affected_rows==1);
    CHECK(id(q(d,"select id from t order by L2_DISTANCE(v,[0,0]) limit 1"))==1);
    d.execute("BEGIN");
    rejects([&] {
      d.execute("BEGIN");
    });
    d.execute("INSERT INTO t VALUES (4,'four',[1,1],0)");
    rejects([&] {
      d.execute("INSERT INTO t VALUES (5,'five',[1,1],0),(2,'dup',[0,0],0)");
    });
    CHECK(q(d,"select * from t where id=5").rows.empty());
    CHECK(q(d,"select * from t where id=4").rows.size()==1);
    d.execute("CREATE TABLE provisional(id BIGINT PRIMARY KEY)");
    d.execute("INSERT INTO provisional VALUES(1)");
    d.execute("DELETE FROM t WHERE id=1");
    d.execute("UPDATE t SET v=[9,9] WHERE id=2");
    CHECK(id(q(d,"select id from t order by L2_DISTANCE(v,[0,0]) limit 1"))==3);
    d.execute("ROLLBACK");
    rejects([&] {
      d.execute("SELECT * FROM provisional");
    });
    CHECK(id(q(d,"select id from t order by L2_DISTANCE(v,[0,0]) limit 1"))==1);
    d.execute("BEGIN");
    d.execute("DELETE FROM u");
    d.execute("UPDATE t SET score=-1 WHERE score>=20");
    d.execute("COMMIT");
    CHECK(q(d,"select * from u").rows.empty());
    CHECK(q(d,"select id from t where score=-1").rows.size()==2);
    d.execute("BEGIN");
    d.execute("INSERT INTO u VALUES(9,'no')");
  }
  {
    Database d(p);
    CHECK(q(d,"select * from u").rows.empty());
    CHECK(q(d,"select * from t").rows.size()==3);
  }
}
void sorting(const std::filesystem::path&p) {
  Database d(p, {
    p.parent_path()/"sort",4096
  });
  d.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(50),n BIGINT)");
  d.execute("BEGIN");
  std::vector<std::pair<int,int>> expected;
  std::mt19937 rng(7);
  for(int i=0;i<180;++i) {
    int n=int(rng()%9);
    expected.emplace_back(n,i);
    d.execute("INSERT INTO t VALUES("+std::to_string(i)+",'abcdefghijklmnopqrstuvwxyz',"+std::to_string(n)+")");
  }
  d.execute("COMMIT");
  std::sort(expected.begin(),expected.end());
  auto r=q(d,"SELECT id FROM t ORDER BY n ASC");
  CHECK(r.info.spill_runs>1);
  CHECK(r.rows.size()==expected.size());
  for(std::size_t i=0;i<expected.size();++i)CHECK(id(r,i)==expected[i].second);
  auto limited=q(d,"SELECT id FROM t ORDER BY n LIMIT 100");
  CHECK(limited.rows.size()==100);
  for(std::size_t i=0;i<100;++i)CHECK(id(limited,i)==expected[i].second);
  CHECK(std::filesystem::is_empty(p.parent_path()/"sort"));
  rejects([&] { d.execute("SELECT id FROM t ORDER BY n", {{}, [](const Row&) { throw std::runtime_error("sink stopped"); }}); });
  CHECK(std::filesystem::is_empty(p.parent_path()/"sort"));
}
void crash(const std::filesystem::path&p) {
  auto pid=fork();
  CHECK(pid>=0);
  if(pid==0) {
    try {
      Database d(p);
      d.execute("BEGIN");
      d.execute("CREATE TABLE a(id BIGINT PRIMARY KEY)");
      d.execute("CREATE TABLE b(id BIGINT PRIMARY KEY)");
      d.execute("INSERT INTO a VALUES(1)");
      d.execute("INSERT INTO b VALUES(1)");
      d.execute("COMMIT");
      d.execute("BEGIN");
      d.execute("DELETE FROM a");
      d.execute("INSERT INTO b VALUES(2)");
      _exit(0);
    }
    catch(...) {
      _exit(1);
    }
  }
  int status=0;
  CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  Database d(p);
  CHECK(q(d,"SELECT * FROM a").rows.size()==1&&q(d,"SELECT * FROM b").rows.size()==1);
}
void edges(const std::filesystem::path&p) {
  Database d(p, {
    p.parent_path()/"sort",4096
  });
  d.execute("CREATE TABLE `T` (`Id` BIGINT PRIMARY KEY,v VECTOR(2),s VARCHAR(4))");
  d.execute("INSERT INTO t VALUES(1,[1,1.490116119384765625e-8],'a'),(2,[1,0],'b'),(-9223372036854775808,[3,4],'中😀'),(9223372036854775807,[-3,-4],'')");
  auto near=q(d,"SELECT id,L2_DISTANCE(v,[0,0]) AS d FROM t ORDER BY d LIMIT 2");
  CHECK(id(near)==2&&id(near,1)==1);
  CHECK(std::get<double>(near.rows[0][1])==1&&std::get<double>(near.rows[1][1])==1);
  CHECK(near.info.distances_computed==4);
  auto sorted=q(d,"SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d");
  CHECK(id(sorted)==2&&id(sorted,1)==1&&id(sorted,2)==INT64_MIN&&id(sorted,3)==INT64_MAX);
  CHECK(sorted.info.distances_computed==4);
  CHECK(q(d,"SELECT * FROM t WHERE id=0 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 8").rows.empty());
  CHECK(id(q(d,"SELECT id FROM t WHERE id=-9223372036854775808"))==INT64_MIN);
  for(auto bad: {
    "INSERT INTO t VALUES(7,[0,0],'x'),(7,[1,1],'y')","UPDATE t SET s='a',s='b'","SELECT * FROM t LIMIT -1","SELECT * FROM t ORDER BY v","SELECT * FROM t WHERE v=[0,0]","SELECT id FROM t ORDER BY L2_DISTANCE(v,[0]) LIMIT 0","SELECT * FROM t /*","INSERT INTO t VALUES(9,[0,0],'unterminated)","CREATE TABLE bad(id VARCHAR(2) PRIMARY KEY)","CREATE TABLE bad(id BIGINT PRIMARY KEY,x BIGINT PRIMARY KEY)","CREATE TABLE bad(id BIGINT PRIMARY KEY,x VARCHAR(0))"
  })rejects([&] {
    d.execute(bad);
  });
  CHECK(q(d,"SELECT * FROM t WHERE id=7").rows.empty());
  d.execute("BEGIN");
  d.execute("CREATE TABLE provisional(id BIGINT PRIMARY KEY)");
  rejects([&] {
    d.execute("CREATE TABLE provisional(id BIGINT PRIMARY KEY)");
  });
  d.execute("ROLLBACK");
  d.execute("CREATE TABLE provisional(id BIGINT PRIMARY KEY)");
  d.execute("INSERT INTO provisional VALUES(1)");
  rejects([&] {
    d.execute("SELECT id FROM t ORDER BY s", {
      {
      },[&](const Row&) {
        throw std::runtime_error("sink failure");
      }
    });
  });
  CHECK(!std::filesystem::exists(p.parent_path()/"sort")||std::filesystem::is_empty(p.parent_path()/"sort"));
  rejects([&] {
    d.execute("SELECT id FROM t", {
      {
      },[&](const Row&) {
        d.execute("DELETE FROM t");
      }
    });
  });
  CHECK(q(d,"SELECT * FROM t").rows.size()==4);
}
void failure(const std::filesystem::path&p) {
  auto pid=fork();
  CHECK(pid>=0);
  if(pid==0) {
    try {
      Database d(p);
      d.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(4))");
      d.execute("INSERT INTO t VALUES(1,'a'),(2,'b'),(3,'c')");
      d.execute("BEGIN");
      d.execute("INSERT INTO t VALUES(4,'d')");
      struct rlimit original {
      };
      CHECK(getrlimit(RLIMIT_FSIZE,&original)==0);
      auto limited=original;
      limited.rlim_cur=std::filesystem::file_size(p)+18*4096;
      CHECK(signal(SIGXFSZ,SIG_IGN)!=SIG_ERR);
      CHECK(setrlimit(RLIMIT_FSIZE,&limited)==0);
      rejects([&] {
        d.execute("UPDATE t SET s='oops'");
      });
      CHECK(setrlimit(RLIMIT_FSIZE,&original)==0);
      CHECK(std::get<std::string>(q(d,"SELECT s FROM t WHERE id=1").rows[0][0])=="a");
      CHECK(q(d,"SELECT * FROM t WHERE id=4").rows.size()==1);
      d.execute("COMMIT");
      _exit(0);
    }
    catch(...) {
      _exit(1);
    }
  }
  int status=0;
  CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  Database d(p);
  CHECK(q(d,"SELECT * FROM t").rows.size()==4);
  CHECK(std::get<std::string>(q(d,"SELECT s FROM t WHERE id=1").rows[0][0])=="a");
}
int main(int argc,char**argv) {
  try {
    CHECK(argc==2);
    const auto* configured=std::getenv("MVDB_TEST_DIR");
    auto base=configured?std::filesystem::path(configured):std::filesystem::path("/data/core-tests");
    std::filesystem::create_directories(base);
    auto pattern=(base/(std::string(argv[1])+"-XXXXXX")).string();
    CHECK(::mkdtemp(pattern.data())!=nullptr);
    auto dir=std::filesystem::path(pattern);
    std::string suite=argv[1];
    if(suite=="codec")codec();
    else if(suite=="sql")sql(dir/"db");
    else if(suite=="sort")sorting(dir/"db");
    else if(suite=="sql_crash")crash(dir/"db");
    else if(suite=="sql_edges")edges(dir/"db");
    else if(suite=="sql_failure")failure(dir/"db");
    else throw std::runtime_error("unknown suite");
    std::filesystem::remove_all(dir);
    std::cout<<"PASS "<<suite<<'\n';
  }
  catch(const std::exception&e) {
    std::cerr<<"FAIL "<<e.what()<<'\n';
    return 1;
  }
}
