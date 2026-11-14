#pragma once
#include "packed_codec.hpp"
#include <bit>

namespace minivectordb::detail {
// Independent lossless byte-plane palettes. No SQL/type or workload knowledge.
// A bounded per-row dictionary is discarded immediately after encoding. Values
// that do not benefit retain their original RLE/raw representation.
inline std::string palette_frame(std::string_view input) {
  std::string out;
  if(input.size()<64 || input.size()>4064)return out;
  out.reserve(input.size()+32);
  for(unsigned i=0;i<4;++i)out+=char((std::uint32_t(input.size())|0x40000000U)>>(8*i));
  std::array<char,1016> lane{};
  std::array<char,1032> runs{};
  std::array<unsigned char,1016> indexes{};
  for(unsigned l=0;l<4;++l) {
    std::size_t n=0;for(auto i=std::size_t(l);i<input.size();i+=4)lane[n++]=input[i];
    const auto run_size=pack_run_frame({lane.data(),n},runs.data())-4;
    unsigned mode=0;std::size_t best=n;
    if(run_size<best){mode=1;best=run_size;}
    std::array<unsigned char,256> lookup;lookup.fill(255);
    std::array<char,16> dictionary{};unsigned count=0;
    for(std::size_t i=0;i<n;++i){const auto c=static_cast<unsigned char>(lane[i]);auto code=lookup[c];
      if(code==255){if(count==16){count=17;break;}code=static_cast<unsigned char>(count);lookup[c]=code;dictionary[count++]=lane[i];}
      indexes[i]=code;
    }
    unsigned bits=count<=16?std::bit_width(count-1):0;
    if(count<=16 && count+(n*bits+7)/8<best){mode=count+1;best=count+(n*bits+7)/8;}
    out+=char(mode);out+=char(best);out+=char(best>>8);
    if(mode==0)out.append(lane.data(),n);
    else if(mode==1)out.append(runs.data()+4,run_size);
    else {
      out.append(dictionary.data(),count);unsigned acc=0,used=0;
      for(std::size_t i=0;i<n;++i){acc|=unsigned(indexes[i])<<used;used+=bits;
        if(used>=8){out+=char(acc);acc>>=8;used-=8;}}
      if(used)out+=char(acc);
    }
  }
  return out;
}
inline bool pack_journal_bytes(std::string_view input,std::string& output) {
  const bool original=pack_bytes(input,output,true);
  auto candidate=palette_frame(input);
  if(!candidate.empty()&&candidate.size()+8<=input.size()&&(!original||candidate.size()<output.size())){
    output=std::move(candidate);return true;
  }
  return original;
}
inline std::string unpack_journal_bytes(std::string_view input) {
  if(input.size()<4 || !(static_cast<unsigned char>(input[3])&64U))return unpack_bytes(input,4064,true);
  auto bad=[](){throw std::runtime_error("corrupt MiniVectorDB palette value");};
  std::uint32_t header=0;for(unsigned i=0;i<4;++i)header|=std::uint32_t(static_cast<unsigned char>(input[i]))<<(8*i);
  const auto expected=std::size_t(header&0x3fffffffU);
  if((header&0x80000000U)||expected<64||expected>4064||input.size()+8>expected)bad();
  std::string output(expected,'\0');std::size_t position=4;
  for(unsigned l=0;l<4;++l){
    if(input.size()-position<3)bad();
    const auto mode=static_cast<unsigned char>(input[position]);
    const auto length=std::size_t(static_cast<unsigned char>(input[position+1]))|(std::size_t(static_cast<unsigned char>(input[position+2]))<<8);position+=3;
    if(length>input.size()-position)bad();const auto encoded=input.substr(position,length);position+=length;
    const auto n=(expected+3-l)/4;
    auto put=[&](std::size_t i,char c){output[l+i*4]=c;};
    if(mode==0){if(length!=n)bad();for(std::size_t i=0;i<n;++i)put(i,encoded[i]);}
    else if(mode==1){std::size_t p=0,count=0;
      while(p<length){const auto control=static_cast<unsigned char>(encoded[p++]);const auto amount=control<128?std::size_t(control)+1:std::size_t(control)-125;
        if(amount>n-count)bad();
        if(control<128){if(amount>length-p)bad();for(std::size_t j=0;j<amount;++j)put(count++,encoded[p++]);}
        else {if(p==length)bad();const auto c=encoded[p++];for(std::size_t j=0;j<amount;++j)put(count++,c);}
      }if(count!=n)bad();
    } else {
      if(mode>17)bad();const unsigned count=mode-1,bits=std::bit_width(count-1);
      if(length!=count+(n*bits+7)/8)bad();
      for(unsigned i=0;i<count;++i)for(unsigned j=0;j<i;++j)if(encoded[i]==encoded[j])bad();
      std::size_t p=count;unsigned acc=0,used=0;
      for(std::size_t i=0;i<n;++i){if(used<bits){acc|=unsigned(static_cast<unsigned char>(encoded[p++]))<<used;used+=8;}
        const auto code=acc&((1U<<bits)-1);if(code>=count)bad();put(i,encoded[code]);acc>>=bits;used-=bits;
      }if(acc||p!=length)bad();
    }
  }
  if(position!=input.size())bad();return output;
}
}
