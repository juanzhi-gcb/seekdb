#include "../src/protocol/jsonl.hpp"
#include <iostream>
#include <random>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
std::size_t scalar_prefix(std::string_view s) {
    std::size_t n=0;for(unsigned char c:s){if(c<32||c=='"'||c=='\\')break;++n;}return n;
}
std::string scalar_quote(std::string_view s) {
    std::string out="\"";constexpr char digits[]="0123456789abcdef";
    for(unsigned char c:s) {
        if(c=='"'||c=='\\'){out+='\\';out+=char(c);}
        else if(c<32){out+="\\u00";out+=digits[c>>4];out+=digits[c&15];}
        else out+=char(c);
    }
    return out+'"';
}
int main(){try{
    std::uint64_t cases=0;std::mt19937 random(0x9a252);
    for(unsigned first=0;first<256;++first)for(unsigned second=0;second<256;++second)
      for(unsigned position=0;position<16;++position) {
        std::string s(33,'a');s[position]=char(first);s[position+1]=char(second);
        CHECK(jsonl::plain_string_prefix(s)==scalar_prefix(s));++cases;
      }
    for(unsigned offset=0;offset<32;++offset)for(unsigned at=0;at<129;++at)for(unsigned byte=0;byte<256;++byte){
      std::string storage(offset+160,'a');storage[offset+at]=char(byte);
      const auto view=std::string_view(storage).substr(offset,129);
      CHECK(jsonl::plain_string_prefix(view)==scalar_prefix(view));++cases;
    }
    const auto page=static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    auto* guard=static_cast<char*>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0));
    CHECK(guard!=MAP_FAILED);CHECK(mprotect(guard+page,page,PROT_NONE)==0);
    std::memset(guard,'a',page);
    for(std::size_t length=1;length<=257;++length)for(unsigned byte=0;byte<256;++byte){
      guard[page-1]=char(byte);auto view=std::string_view(guard+page-length,length);
      CHECK(jsonl::plain_string_prefix(view)==scalar_prefix(view));++cases;
    }
    CHECK(munmap(guard,page*2)==0);
    for(unsigned trial=0;trial<2000;++trial) {
        std::string s;for(unsigned i=0,n=random()%4096;i<n;++i)s+=char(random());
        CHECK(jsonl::plain_string_prefix(s)==scalar_prefix(s));
        CHECK(jsonl::quote(s)==scalar_quote(s));++cases;
    }
    for(unsigned padding=0;padding<33;++padding) {
        for(const std::string suffix:{std::string("中😀é"),std::string("\0\n\r\t\b\f\\\"",8),std::string(65536,'x')}) {
            const auto value=std::string(padding,'x')+suffix;
            const auto raw=scalar_quote(value);const auto frame="{\"id\":"+raw+",\"sql\":"+raw+"}";
            auto parsed=jsonl::Parser(frame).parse();CHECK(parsed.at("id").text==value);
            CHECK(parsed.at("sql").text==value);CHECK(parsed.at("id").raw==raw);
            CHECK(parsed.at("sql").raw.data()>=frame.data()&&parsed.at("sql").raw.data()<frame.data()+frame.size());
        }
        for(unsigned byte=0;byte<32;++byte) {
            const auto frame="{\"id\":\""+std::string(padding,'a')+char(byte)+"\"}";
            bool failed=false;try{(void)jsonl::Parser(frame).parse();}catch(const std::exception&){failed=true;}CHECK(failed);
        }
    }
    for(const std::string frame:{"{\"id\":1e309}","{\"id\":\"\\ud800\"}","{\"id\":\"\\udc00\"}","{\"id\":\"\\uZZZZ\"}","{\"id\":\"\\x\"}","{\"id\":\"unterminated"}) {
        bool failed=false;try{(void)jsonl::Parser(frame).parse();}catch(const std::exception&){failed=true;}CHECK(failed);
    }
    const std::string tiny="{\"id\":1e-9999}";CHECK(jsonl::Parser(tiny).parse().at("id").raw=="1e-9999");
    std::cout<<"PASS JSON byte-prefix/escaping oracle, UTF-8 controls and borrowed lexemes: "<<cases<<" cases\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
