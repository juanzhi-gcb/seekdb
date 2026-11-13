#include "../src/sql/sql.hpp"
#include "minivectordb/database.hpp"
#include <filesystem>
#include <iostream>
#include <sstream>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
struct Result {std::vector<std::string> columns;std::vector<Row> rows;std::uint64_t affected=0;std::string error;bool operator==(const Result&)const=default;};
Result query(Database& db,std::string_view text){Result r;try{auto i=db.execute(text,{[&](const auto& c){r.columns=c;},[&](const Row& row){r.rows.push_back(row);}});r.affected=i.affected_rows;}catch(const Error&e){r.error=std::to_string(int(e.code()))+":"+e.what();}return r;}
int main(){try{
 const auto root=std::filesystem::path(std::getenv("MVDB_TEST_DIR")?std::getenv("MVDB_TEST_DIR"):"/tmp")/("streamlexer-"+std::to_string(getpid()));std::filesystem::create_directories(root);
 Options a,b;a.journal_store=b.journal_store=true;a.streaming_sql_tokens=false;b.streaming_sql_tokens=true;
 Database old(root/"old",a),stream(root/"stream",b);
 auto check=[&](std::string s){auto x=query(old,s),y=query(stream,s);CHECK(x==y);std::fill(s.begin(),s.end(),'x');};
 for(auto s:{"CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(1000),v VECTOR(3))","INSERT INTO t VALUES(1,'a''b','[1,2,3]'),(2,'/*x*/ -- y',[0,0,0])","SELECT * FROM t ORDER BY id","BEGIN","UPDATE t SET s='changed' WHERE id=1","ROLLBACK","SELECT id,L2_DISTANCE(v,'[+1.0,2e0,3]') AS d FROM t ORDER BY d LIMIT 2","CREATE VECTOR INDEX tv ON t(v)","SELECT * FROM t WHERE s='a''b'","BEGIN","DELETE FROM t WHERE id=2","INSERT INTO t VALUES(3,'new','[0,-0,1]')","COMMIT","SELECT * FROM t ORDER BY id"})check(s);
 // Whole-input lexical errors must precede earlier grammar/schema errors.
 for(auto s:{"CREATE TABLE bad(id UNSUPPORTED) 'unterminated","CREATE TABLE bad(id BIGINT PRIMARY KEY,PRIMARY KEY(id)) /* unterminated","INSERT INTO missing VALUES(1) @","nonsense 'unterminated","BEGIN extra /* unterminated","INSERT INTO t VALUES(9,'x','[1e999,2,3]') trailing","SELECT id FROM t WHERE id=1e999; @","SELECT 'a''b' FROM t","","-- comment only","/*x*/ SELECT * FROM t; -- end"})check(s);
 for(int i=0;i<200;++i){auto value=std::string(std::size_t(i*3),'z');check("INSERT INTO t VALUES("+std::to_string(i+10)+",'"+value+"',[1,2,3])");if(i%13==0)check("SELECT * FROM t WHERE id="+std::to_string(i+10));}
 check("SELECT * FROM t ORDER BY id");
 // Both APIs must return owning AST literals after the source is destroyed.
 sql::Statement parsed;{std::string text="INSERT INTO t VALUES(10000,'persistent text','[1,2,3]')";parsed=sql::parse_stream(text);std::fill(text.begin(),text.end(),'!');}
 CHECK(parsed.values[0][1].text=="persistent text");CHECK(parsed.values[0][2].text=="[1,2,3]");
 // Internal borrows must become independent owned values during binding.
 Schema schema{7,"t",{{"id",ColumnType::Bigint,0},{"s",ColumnType::Varchar,1000},{"v",ColumnType::Vector,3}},0};
 sql::Bound bound;{std::string text="INSERT INTO t VALUES(11000,'owned after bind','[1,2,3]')";auto ast=sql::parse_stream(text,true);CHECK(ast.values[0][1].borrowed.data()!=nullptr);bound=sql::bind(ast,schema);std::fill(text.begin(),text.end(),'!');}
 CHECK(std::get<std::string>(bound.insert_rows[0][1])=="owned after bind");CHECK(std::get<std::vector<float>>(bound.insert_rows[0][2])==std::vector<float>({1,2,3}));
 // The first user callback may invalidate input after binding; execution must
 // use bound values, never dereference a borrowed literal again.
 std::string select="SELECT id FROM t WHERE s='new' ORDER BY id";std::vector<Row> result;
 stream.execute(select,{[&](const auto&){std::fill(select.begin(),select.end(),'!');},[&](const Row& row){result.push_back(row);}});CHECK(result.size()==1 && std::get<std::int64_t>(result[0][0])==3);

 std::filesystem::remove_all(root);std::cout<<"streaming lexer: SQL results, mutation state, lexical precedence and literal ownership passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
