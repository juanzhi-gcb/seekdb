#include "../src/storage/batch_directory.hpp"
#include <random>
#include <vector>
#include <iostream>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" at "+std::to_string(__LINE__));}while(false)
int main(){try{std::mt19937 random(71830);std::size_t checks=0;
 for(unsigned trial=0;trial<120;++trial){std::vector<Store::Mutation> rows;std::vector<const Store::Mutation*> refs;const unsigned count=2+random()%127;
  const std::int64_t base=trial%3==0?INT64_MIN:(trial%3==1?-900:INT64_MAX-65535);
  for(unsigned i=0;i<count;++i){std::string value;const auto size=random()%2000;for(unsigned k=0;k<size;++k)value+=char(trial%2?random():random()%8);rows.push_back({base+std::int64_t(i*17),random()%7?std::optional(value):std::nullopt});}
  for(auto& r:rows)refs.push_back(&r);std::string image;CHECK(detail::batchdir::encode(7,UINT64_MAX,refs,image));auto d=detail::batchdir::parse(image,image.size());CHECK(d.space==7&&d.previous==UINT64_MAX&&d.count==rows.size());
  for(unsigned i=0;i<count;++i){CHECK(d.entries[i].key==rows[i].key);CHECK(detail::batchdir::value(d,image,i)==rows[i].value);auto h=detail::batchdir::header_from_checked_pages(image,image.size());CHECK(detail::batchdir::value(detail::batchdir::entry(h,image,i),image)==rows[i].value);++checks;}
  for(unsigned cut=0;cut<d.prefix;++cut){bool failed=false;try{(void)detail::batchdir::parse(std::string_view(image).substr(0,cut),image.size());}catch(const std::runtime_error&){failed=true;}CHECK(failed);++checks;}
  for(unsigned i=0;i<d.prefix;++i){auto broken=image;broken[i]^=1;bool failed=false;try{(void)detail::batchdir::parse(broken,broken.size());}catch(const std::runtime_error&){failed=true;}CHECK(failed);++checks;}
  if(d.bytes>d.prefix){bool failed=false;try{(void)detail::batchdir::parse(image,image.size()-1);}catch(const std::runtime_error&){failed=true;}CHECK(failed);}
  for(unsigned i=0;i<count;++i)if(d.entries[i].length){auto broken=image;broken[d.entries[i].offset]^=1;bool failed=false;try{(void)detail::batchdir::value(d,broken,i);}catch(const std::runtime_error&){failed=true;}CHECK(failed);++checks;}
 }
 for(auto delta:{32768LL,9223372036854775807LL}){Store::Mutation a{0,"a"},b{delta,"b"};const Store::Mutation* rs[]{&a,&b};std::string out;CHECK(!detail::batchdir::encode(0,0,rs,out));}
 {Store::Mutation a{0,std::string(Store::max_batch_bytes+1,'x')},b{1,"b"};const Store::Mutation* rs[]{&a,&b};std::string out;CHECK(!detail::batchdir::encode(0,0,rs,out));}
 std::cout<<"batch directory codec exactness/corruption checks: "<<checks<<"\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
