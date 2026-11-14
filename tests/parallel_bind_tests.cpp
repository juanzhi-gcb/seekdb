#include "../src/sql/sql.hpp"
#include <iostream>
#include <algorithm>
#include <set>
#include <cfenv>
#include <optional>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
struct Result {std::optional<ErrorCode> error;std::string message;std::vector<std::string> bytes;};
Result result(const sql::Statement& statement,const Schema& schema,bool serial) {
 Result out;try{
  if(!serial){for(const auto&row:sql::bind(statement,schema).insert_rows)out.bytes.push_back(encode_row(schema,row));}
  else {std::set<std::int64_t> keys;auto one=statement;for(const auto&values:statement.values){one.values={values};auto rows=sql::bind(one,schema).insert_rows;const auto pk=std::get<std::int64_t>(rows.at(0).at(schema.primary));if(!keys.insert(pk).second)throw Error(ErrorCode::Constraint,"duplicate primary key within INSERT");out.bytes.push_back(encode_row(schema,rows.at(0)));}}
 }catch(const Error&e){out.error=e.code();out.message=e.what();out.bytes.clear();}return out;
}
void same(const sql::Statement& s,const Schema& schema) {auto a=result(s,schema,true),b=result(s,schema,false);CHECK(a.error==b.error);CHECK(a.message==b.message);CHECK(a.bytes==b.bytes);}
sql::Literal number(std::string s){sql::Literal l;l.kind=sql::Literal::Number;l.text=std::move(s);return l;}
sql::Literal string(std::string s){sql::Literal l;l.kind=sql::Literal::String;l.text=std::move(s);return l;}
int main(){try{
 Schema schema;schema.name="t";schema.primary=0;schema.columns={{"id",ColumnType::Bigint,0},{"v",ColumnType::Vector,128},{"body",ColumnType::Varchar,64}};
 std::string vector="[";const char* values[]={".1","-0","16777217","-1e-999","3.4028235e38","1.000000059604644775390625"};for(unsigned i=0;i<128;++i){if(i)vector+=',';vector+=values[i%6];}vector+=']';
 unsigned cases=0;const int saved=std::fegetround();
 for(int rounding:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}){
  CHECK(std::fesetround(rounding)==0);
  for(unsigned n:{63u,64u,65u,100u,257u}){
   sql::Statement s;s.kind=sql::Statement::Insert;
   for(unsigned i=0;i<n;++i)s.values.push_back({number(std::to_string(i)),string(vector),string("payload-"+std::to_string(i))});
   same(s,schema);++cases;
   for(unsigned bad:{0u,1u,n/2,n-1}){
    auto x=s;x.values[bad][1]=string("[1e999,@]");same(x,schema);++cases;
    x=s;x.values[bad].pop_back();same(x,schema);++cases;
    if(bad){x=s;x.values[bad][0]=number("0");if(bad+1<n)x.values[bad+1][1]=string("[nan]");same(x,schema);++cases;}
   }
   auto x=s;x.insert_columns={"v","body","id"};for(auto& row:x.values)std::rotate(row.begin(),row.begin()+1,row.end());same(x,schema);++cases;
  }
 }
 CHECK(std::fesetround(saved)==0);std::cout<<cases<<" parallel/serial cases: bitwise rows, column permutation, first-error ordering, four rounding modes passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
