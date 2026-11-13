#include "../src/storage/lossless_words.hpp"
#include "../src/storage/lossless_words_reference.hpp"
#include <random>
#include <iostream>
using namespace minivectordb::detail;
int main(){std::mt19937 rng(34251);unsigned cases=0;
 for(unsigned n=0;n<=4064;++n)for(unsigned form=0;form<5;++form){
  std::string s(n,'x');for(auto& c:s)c=char(rng());
  if(form==1)s.assign(n,'\0');
  if(form>=2)for(unsigned p=form-2;p+4<=n;p+=4){float x=form==2?float(int(rng()%4097)-2048)/128.f:form==3?half_decode(static_cast<std::uint16_t>(rng()%0x7c00)):std::bit_cast<float>(static_cast<std::uint32_t>(rng()));std::memcpy(s.data()+p,&x,4);}
  std::string a,b;bool aa=halfwords_reference::transform(s,a),bb=halfwords::transform(s,b);
  if(aa!=bb||(aa&&a!=b)||halfwords_reference::choose_alignment(s)!=halfwords::choose_alignment(s))throw std::runtime_error("encoding parity");
  if(bb&&halfwords::inverse(b,s.size())!=s)throw std::runtime_error("roundtrip");++cases;
 }
 std::cout<<"PASS compact writer legacy byte parity and roundtrip "<<cases<<" cases\n";
}
