#include "../src/sql/typed_scan_cache.hpp"
#include "../src/vector/certified_candidates.hpp"
#include <random>
#include <iostream>
#include <stdexcept>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
int main(){try{
 std::mt19937 rng(99173);
 for(int mode:{FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO}){
  CHECK(std::fesetround(mode)==0);
  for(int power:{-24,-7,0,9})for(int extent:{1,63,127,255,1023,2048,4096}){
   constexpr std::size_t rows=531,dim=129;
   std::vector<float> values(rows*dim);detail::SharedCodePlan plan;
   for(auto& x:values){x=std::ldexp(float(int(rng()%unsigned(2*extent+1))-extent),power);plan.observe(x);}
   CHECK(plan.width()<=7);detail::SharedCodes shared;shared.allocate(rows,dim,plan);
   for(std::size_t r=0;r<rows;++r)CHECK(shared.append(r,{values.data()+r*dim,dim}));
   for(std::size_t r=0;r<rows;++r)for(std::size_t d=0;d<dim;++d)CHECK(std::bit_cast<std::uint32_t>(shared.value(r,d))==std::bit_cast<std::uint32_t>(values[r*dim+d]));
   // Exercise in-place cluster primitives before transposing shared codes.
   std::vector<std::byte> scratch(shared.row_bytes());shared.save(0,scratch.data());shared.move(0,530);shared.load(530,scratch.data());
   for(std::size_t d=0;d<dim;++d)std::swap(values[d],values[530*dim+d]);
   shared.transpose();
   for(std::size_t r=0;r<rows;++r)for(std::size_t d=0;d<dim;++d)CHECK(std::bit_cast<std::uint32_t>(shared.value(r,d))==std::bit_cast<std::uint32_t>(values[r*dim+d]));
   if(mode==FE_TONEAREST){detail::Int8BoundCache bounds;
    CHECK(bounds.build_values(rows,dim,rows*16,[&](auto r,auto d){return shared.value(r,d);},true,shared.codes(),shared.scale()));
    CHECK(bounds.bytes()==rows/16*16*16);
    for(unsigned q=0;q<8;++q){std::span<const float> query(values.data()+(rng()%rows)*dim,dim);auto prepared=bounds.query(query);CHECK(prepared.valid);
     for(std::size_t r=0;r+16<=rows;r+=16){std::array<double,16> lower{};bounds.lower_scores(prepared,r,lower);
      for(std::size_t lane=0;lane<16;++lane){double exact=0;for(std::size_t d=0;d<dim;++d){double delta=double(query[d])-values[(r+lane)*dim+d];exact+=delta*delta;}
       // A radius equal to the authoritative exact distance must not reject.
       CHECK(lower[lane]<=bounds.exclusion_upper(prepared,std::sqrt(exact)));
      }
     }
    }
   }
  }
 }
 std::fesetround(FE_TONEAREST);
 for(float x:{-0.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){detail::SharedCodePlan p;p.observe(x);CHECK(p.width()==99);}
 detail::SharedCodePlan wide;wide.observe(1.f);wide.observe(std::nextafter(1.f,2.f));CHECK(wide.width()==99);
 Schema schema{91,"t",{{"id",ColumnType::Bigint,0},{"v",ColumnType::Vector,128},{"g",ColumnType::Bigint,0}},0};
 sql::TypedScanCache cache;std::vector<std::vector<float>> vectors(531,std::vector<float>(128));
 for(auto& v:vectors)for(auto& x:v)x=float(int(rng()%4097)-2048)/128;
 for(unsigned cycle=0;cycle<4;++cycle){cache.invalidate();cache.start(schema,0,1,2*1024*1024,true,{},true);
  for(auto& v:vectors)cache.observe_row({},v);cache.finish();cache.start(schema,0,1,1024*1024,true,{},true);
  CHECK(cache.shared_codes()==detail::Int8BoundCache::available());
  for(std::size_t r=0;r<vectors.size();++r)cache.append(vectors[r],[&](auto c){return c==0?std::int64_t(r):std::int64_t(r%7);});
  cache.finish(true,true,true,true);CHECK(cache.ready(91,0,1));std::vector<float> scratch;
  for(std::size_t r=0;r<cache.size();++r){auto v=cache.read_vector(r,scratch);for(std::size_t d=0;d<128;++d)CHECK(v[d]==vectors[cache.key(r)][d]);}
  if(detail::Int8BoundCache::available()){CHECK(cache.integer_cache());CHECK(cache.bytes()<531*(128*2+32));}
 }
 std::cout<<"shared codes: exact lattice, rounding modes, tails, cluster, bounds, invalidation and fallback passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
