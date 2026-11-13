#include "../src/sql/sql.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <random>
#include <sstream>
#include <iomanip>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
std::string evaluate(std::string text,std::size_t dimension,bool fast){sql::Literal literal;literal.kind=sql::Literal::String;literal.text=std::move(text);try{auto value=sql::bind_literal(literal,{"v",ColumnType::Vector,static_cast<std::uint32_t>(dimension)},fast);std::string out;for(float x:std::get<std::vector<float>>(value))out+=std::to_string(std::bit_cast<std::uint32_t>(x))+",";return out;}catch(const Error&e){return "ERROR:"+std::to_string(int(e.code()))+":"+e.what();}}
int main(){try{std::mt19937_64 rng(71925);std::size_t count=0;auto check=[&](const std::string& value,std::size_t dimension=1){auto a=evaluate(value,dimension,false),b=evaluate(value,dimension,true);if(a!=b){std::cerr<<value<<"\n"<<a<<"\n"<<b<<'\n';throw std::runtime_error("decimal differential");}++count;};
 const auto rounding=std::fegetround();
 for(int mode:{FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO}){CHECK(std::fesetround(mode)==0);
  for(auto value:{"[-0]","[+0.0]","[1.]","[.1]","[-.5]","[1e22]","[1e-22]","[9.9999999999999e22]","[9007199254740993]","[1.0000000596046448]","[3.4028235e38]","[3.4028236e38]","[-1e-999]","[1e999,foo]","[1e999]","[1e999,2]","[1 /*comment*/,2]","[NaN]","[-nan]","[+inf]","[1e]","[1,]","[1,,2]","[]","/*x*/ [1]","[1] /*x*/","[1\v]"}){check(value);check(value,3);}
  for(int n=0;n<12000;++n){auto integer=std::to_string(rng()%1000000000000000ULL);if(rng()%2)integer.insert(rng()%(integer.size()+1),".");if(rng()%2)integer="-"+integer;if(rng()%2)integer+="e"+std::to_string(int(rng()%51)-25);check("["+integer+"]");}
 }
 CHECK(std::fesetround(rounding)==0);std::cout<<"{\"differential_cases\":"<<count<<",\"rounding_modes\":4,\"signed_zero_and_error_precedence\":true}\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
