// Reuse the independent SQL oracle and range fixtures from this image's source.
// Compile with -I/src/tests -I/src/include -I/src and -Wl,--wrap=pread.
#define main typed_scan_fixture_main
#include <typed_scan_tests.cpp>
#undef main
#include "src/vector/validated.hpp"

void range_isa(const std::filesystem::path& root,bool wide) {
  std::filesystem::create_directories(root);
  const auto items=range_fixture();
  for(bool indexed:{false,true}) {
    auto options=settings(262144);
    options.clustered_range_scan=true;options.transposed_scan_cache=true;
    options.simd_vector_kernel=true;options.batched_vector_kernel=true;
    options.wide_simd_vector_kernel=wide;
    Database db(root/(indexed?"indexed.db":"unindexed.db"),options);
    range_seed(db,items);if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
    // Odd group sizes, 97 rows, and shifted ranges exercise SIMD heads/tails.
    for(const auto& [where,predicate]:std::vector<std::pair<std::string,Filter>>{
      {"g=-9223372036854775808",[](const Item&i){return i.g==INT64_MIN;}},
      {"g=9223372036854775807",[](const Item&i){return i.g==INT64_MAX;}},
      {"g=0",[](const Item&i){return i.g==0;}},
      {"g=3",[](const Item&i){return i.g==3;}},
      {"g<0",[](const Item&i){return i.g<0;}},
      {"g<=0",[](const Item&i){return i.g<=0;}},
      {"g>0",[](const Item&i){return i.g>0;}},
      {"g>=0",[](const Item&i){return i.g>=0;}},
      {"g>=-1 AND g<2",[](const Item&i){return i.g>=-1&&i.g<2;}},
      {"g=0 AND g=1",[](const Item&){return false;}},
      {"label=''",[](const Item&i){return i.label.empty();}},
      {"label='"+std::string("a\0b",3)+"'",[](const Item&i){return i.label==std::string("a\0b",3);}},
      {"label='😀'",[](const Item&i){return i.label=="😀";}},
      {"label='absent'",[](const Item&i){return i.label=="absent";}},
      {"label>='a' AND label<'😀'",[](const Item&i){return i.label>="a"&&i.label<"😀";}},
      {"label>'中' AND label<'a'",[](const Item&){return false;}}})
        check_range(db,items,where,predicate,predicate);
    check_range(db,items,"g>=0 AND h=1",[](const Item&i){return i.g>=0&&i.h==1;},
                [](const Item&i){return i.g>=0;});
    check_range(db,items,"label>='a' AND h=1",[](const Item&i){return i.label>="a"&&i.h==1;},
                [](const Item&i){return i.label>="a";});
  }
}
int main(int argc,char** argv) {
  try {
    CHECK(argc==2);const std::filesystem::path root(argv[1]);
    CHECK(!std::filesystem::exists(root));
    const bool avx2=detail::has_avx2(),avx512=detail::has_avx512();
    if(avx2)range_isa(root/"avx2",false);
    if(avx512)range_isa(root/"avx512",true);
    std::cout<<"{\"avx2_supported\":"<<(avx2?"true":"false")
             <<",\"avx512_supported\":"<<(avx512?"true":"false")
             <<",\"avx2_range\":\""<<(avx2?"passed":"unsupported")
             <<"\",\"avx512_range\":\""<<(avx512?"passed":"unsupported")
             <<"\",\"indexed_modes_per_supported_isa\":2,\"range_shapes_per_mode\":18}\n";
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
