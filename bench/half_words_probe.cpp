#define main old_codec_main
#include "codec_probe.cpp"
#undef main
#include "half_words.hpp"
int main(int argc,char**argv){try {
 const bool fast=false;
 std::vector<std::vector<Record>> corpora;if(argc==2)corpora.push_back(load(argv[1]));else{auto rows=general();std::mt19937 gen(20261113);for(auto& row:rows)for(unsigned d=0;d<128;++d){float x=float(int(gen()%4097)-2048)/128;std::memcpy(row.raw.data()+25+d*4,&x,4);}for(unsigned i=0;i<rows.size();++i){auto& raw=rows[i].raw;raw.resize(537);std::string body="record-"+std::to_string(i)+"-"+std::string(256,char(97+i%26));put(raw,body.size(),4);raw+=body;}corpora.push_back(std::move(rows));}corpora.push_back(general());
 for(int round=0;round<3;++round)for(unsigned ci=0;ci<corpora.size();++ci)for(unsigned phase=0;phase<2;++phase){const auto mode=round%2?1-phase:phase;const auto& rows=corpora[ci];std::vector<std::string> coded;std::vector<bool> packed,half;std::size_t total=0;const auto t=Clock::now();
  for(const auto& row:rows){std::string bytes;bool ok;bool transformed=false;
   if(mode){std::string compact;ok=halfwords::transform(row.raw,compact);transformed=ok;if(ok){std::string runs;bool rle=detail::pack_bytes(compact,runs,false);bytes=char(rle);bytes+=rle?runs:compact;}else ok=detail::pack_bytes(row.raw,bytes,true);}
   else ok=detail::pack_bytes(row.raw,bytes,true);
   if(!ok)bytes=row.raw;total+=bytes.size()+48;coded.push_back(std::move(bytes));packed.push_back(ok);half.push_back(transformed);
  }const auto enc=Clock::now();std::mt19937 rng(20261113);for(unsigned i=0;i<rows.size();++i){auto ix=rng()%rows.size();auto raw=packed[ix]?(half[ix]?halfwords::inverse(coded[ix][0]?detail::unpack_bytes(std::string_view(coded[ix]).substr(1),4064,false):coded[ix].substr(1),rows[ix].raw.size()):detail::unpack_bytes(coded[ix],4064,true)):coded[ix];require(raw==rows[ix].raw,"roundtrip");}
  const auto end=Clock::now();std::cout<<"{\"round\":"<<round<<",\"corpus\":"<<ci<<",\"candidate\":"<<mode<<",\"native\":"<<fast<<",\"bytes\":"<<total+8192<<",\"encode_ms\":"<<std::chrono::duration<double,std::milli>(enc-t).count()<<",\"random_decode_ms\":"<<std::chrono::duration<double,std::milli>(end-enc).count()<<"}\n";
 }
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
