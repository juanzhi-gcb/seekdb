#include "../src/storage/batch_worker.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace minivectordb::detail;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
int main(){try{BatchWorker worker;std::array<int,128> values{};unsigned throws=0;
 for(int round=0;round<1000;++round){auto job=[&]{for(unsigned i=64;i<128;++i)values[i]=round+int(i);if(round%13==0)throw std::runtime_error("expected worker failure");};worker.start(job);for(unsigned i=0;i<64;++i)values[i]=round+int(i);bool failed=false;try{worker.finish();}catch(const std::runtime_error&){failed=true;++throws;}CHECK(failed==(round%13==0));for(unsigned i=0;i<128;++i)CHECK(values[i]==round+int(i));}
 CHECK(throws==77);std::cout<<"1000 joined tasks, 77 exception/reuse paths passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
