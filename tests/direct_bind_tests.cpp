#define main original_insert_literal_main
#include "insert_literal_tests.cpp"
#undef main
#include <cfenv>
int main() {
  try {
    if(original_insert_literal_main()!=0)return 1;
    const auto saved=std::fegetround();std::size_t compared=0;
    const std::string edges[]{"[1e39,1e309]","[1e39,@]","[1e39,1]","[1e309,1]",
      "[1e-999,-1e-999,0,-0]","[3.4028235e38,-3.4028235e38]",
      "[3.4028235677973366e38]","[1.000000059604644775390625]",
      "[1.0000000596046447753906249]","[16777217,-16777219]",
      "[1.401298464324817e-45,-1.401298464324817e-45]","[1,2,3,4,1e309]",
      "[1e39,1e309,@]","[1e39,1e309,/*tail*/2]","[]","[1,,2]"};
    for(auto mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
      CHECK(std::fesetround(mode)==0);
      for(const auto& text:edges)for(unsigned dimension:{0u,1u,2u,4u,5u}){same(text,dimension);++compared;}
      std::mt19937 rng(20261115);
      for(unsigned round=0;round<1000;++round) {
        const auto count=1+rng()%256;std::string text="[";
        for(unsigned i=0;i<count;++i){if(i)text+=',';text+=(rng()%2?"-":"+")+std::to_string(rng())+"."+std::to_string(rng())+"e"+std::to_string(int(rng()%90)-50);}
        text+=']';same(text,count);++compared;
        if(round%8==0){same(text,count-1);++compared;}
      }
    }
    CHECK(std::fesetround(saved)==0);
    std::cout<<"PASS direct float binding: "<<compared<<" additional rounding/error-priority comparisons\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
