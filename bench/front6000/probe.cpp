#include "sql/sql.hpp"
#include <chrono>
#include <iostream>
#include <string>
#include <iomanip>
#include <sstream>
#include <cmath>
using namespace minivectordb;
int main(){
 Schema schema{1,"t",{{"id",ColumnType::Bigint,0},{"g",ColumnType::Bigint,0},{"v",ColumnType::Vector,128},{"body",ColumnType::Varchar,512}},0};
 for(int form=0;form<2;++form){
 std::string vector="[";for(int i=0;i<128;++i){if(i)vector+=',';if(form){std::ostringstream s;s<<std::setprecision(17)<<std::sin(double(i)*0.31);vector+=s.str();}else vector+=std::to_string((i%31-15)*0.125);}vector+=']';
 const std::string select="SELECT id,L2_DISTANCE(v,'"+vector+"') AS d,body FROM t WHERE g=3 ORDER BY d LIMIT 10;";
 std::string insert="INSERT INTO t VALUES ";for(int r=0;r<100;++r){if(r)insert+=',';insert+="("+std::to_string(r)+",3,'"+vector+"','"+std::string(256,'x')+"')";}insert+=';';
 for(int round=0;round<3;++round)for(int kind=0;kind<2;++kind)for(int stage=0;stage<3;++stage){const auto& text=kind?select:insert;const int repeats=kind?10000:1000;std::size_t digest=0;
 auto parse=[&](){
#ifdef MVDB_STREAM_PARSE
 return sql::parse_stream(text,true);
#else
 return sql::parse(text);
#endif
 };
 auto parsed_once=parse();auto at=std::chrono::steady_clock::now();
 for(int r=0;r<repeats;++r){
  if(stage==1){auto bound=sql::bind(parsed_once,schema);digest+=bound.insert_rows.size()+bound.projection.size();if(!bound.insert_rows.empty())digest+=std::get<std::vector<float>>(bound.insert_rows.back()[2]).size();}
  else {auto parsed=parse();digest+=parsed.table.size()+parsed.projection.size()+parsed.values.size();if(stage==2){auto bound=sql::bind(parsed,schema);digest+=bound.insert_rows.size()+bound.projection.size();}}
 }
 const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-at).count();std::cout<<"{\"form\":\""<<(form?"long":"short")<<"\",\"kind\":\""<<(kind?"SELECT":"INSERT")<<"\",\"stage\":\""<<(stage==0?"parse":stage==1?"bind":"parse_bind")<<"\",\"round\":"<<round<<",\"ms\":"<<ms<<",\"repeats\":"<<repeats<<",\"digest\":"<<digest<<"}\n";
 }
 }
}
