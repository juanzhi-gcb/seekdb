#include "storage/batch_directory.hpp"
#include "minivectordb/row_codec.hpp"
#include <random>
#include <numeric>
#include <iostream>
#include <chrono>
using namespace minivectordb;
using namespace minivectordb::detail;
using Clock=std::chrono::steady_clock;
struct Legacy {std::string bytes;std::vector<std::size_t> offsets;};
Legacy legacy(std::span<const Store::Mutation* const> rows){Legacy out;std::string packed;std::uint64_t previous=0;for(auto* row:rows){out.offsets.push_back(out.bytes.size());std::string_view p=*row->value;unsigned flag=0;if(pack_bytes(p,packed,true,true)){p=packed;flag=1;}char h[48]{};std::memcpy(h,"JREC",4);batchdir::put(h+4,flag,4);batchdir::put(h+8,7,8);batchdir::put(h+16,std::bit_cast<std::uint64_t>(row->key),8);batchdir::put(h+24,previous,8);batchdir::put(h+32,p.size(),8);batchdir::put(h+40,batchdir::crc(p),4);batchdir::put(h+44,batchdir::crc({h,44}),4);previous=8192+out.bytes.size();out.bytes.append(h,48);out.bytes.append(p);}return out;}
std::string old_value(const Legacy& x,unsigned i){const auto at=x.offsets[i];const auto h=std::string_view(x.bytes).substr(at,48);if(batchdir::get(h.data()+44,4)!=batchdir::crc(h.substr(0,44)))throw std::runtime_error("old header");auto body=std::string_view(x.bytes).substr(at+48,batchdir::get(h.data()+32,8));if(batchdir::crc(body)!=batchdir::get(h.data()+40,4))throw std::runtime_error("old body");return batchdir::get(h.data()+4,4)?unpack_bytes(body,4064,true):std::string(body);}
int main(){try{std::mt19937 random(9173);for(unsigned corpus=0;corpus<2;++corpus)for(unsigned varchar=0;varchar<2;++varchar){
 Schema schema{7,"t",{{"id",ColumnType::Bigint,0},{"g",varchar?ColumnType::Varchar:ColumnType::Bigint,varchar?32U:0U},{"v",ColumnType::Vector,128},{"body",ColumnType::Varchar,512}},0};
 std::vector<Store::Mutation> rows;for(int i=0;i<10000;++i){std::vector<float> vector;for(int d=0;d<128;++d)vector.push_back(corpus?float(double(random())/4294967296.):float(int(random()%4097)-2048)/128.f);std::string body="record-"+std::to_string(i)+"-"+std::string(256,char('a'+i%26));if(corpus)for(auto& c:body)c=char(33+random()%90);Row row{std::int64_t(i),varchar?Cell("category-"+std::to_string(i%10)):Cell(std::int64_t(i%10)),std::move(vector),std::move(body)};rows.push_back({i,encode_row(schema,row)});}
 std::shuffle(rows.begin(),rows.end(),random);std::vector<std::vector<const Store::Mutation*>> batches;for(std::size_t b=0;b<rows.size();b+=100){std::vector<const Store::Mutation*> r;for(unsigned i=0;i<100;++i)r.push_back(&rows[b+i]);std::sort(r.begin(),r.end(),[](auto a,auto b){return a->key<b->key;});batches.push_back(std::move(r));}
 for(unsigned round=0;round<3;++round){std::vector<Legacy> olds;std::vector<std::string> news;std::size_t old_bytes=8192,new_bytes=8192;double old_encode=0,new_encode=0;
 auto build_old=[&]{auto at=Clock::now();for(auto& b:batches){auto image=legacy(b);old_bytes+=((image.bytes.size()+4047)/4048)*4096;olds.push_back(std::move(image));}old_encode=std::chrono::duration<double,std::milli>(Clock::now()-at).count();};
 auto build_new=[&]{auto at=Clock::now();for(auto& b:batches){std::string image;if(!batchdir::encode(7,0,b,image))throw std::runtime_error("batch ineligible");new_bytes+=((image.size()+4047)/4048)*4096;news.push_back(std::move(image));}new_encode=std::chrono::duration<double,std::milli>(Clock::now()-at).count();};
 if(round%2){build_new();build_old();}else{build_old();build_new();}

 std::vector<batchdir::Directory> dirs;for(auto& image:news)dirs.push_back(batchdir::parse(image,image.size()));for(unsigned b=0;b<100;++b)for(unsigned i=0;i<100;++i)if(old_value(olds[b],i)!=*batches[b][i]->value || batchdir::value(dirs[b],news[b],i)!=batches[b][i]->value)throw std::runtime_error("roundtrip");
 std::size_t sum=0;double access[5]{};for(unsigned mode=0;mode<5;++mode){std::mt19937 access_random(1007);auto a=Clock::now();for(unsigned i=0;i<20000;++i){const auto b=access_random()%100,n=access_random()%100;if(mode==0)sum+=old_value(olds[b],n).size();else if(mode==1)sum+=batchdir::value(dirs[b],news[b],n)->size();else if(mode==2){auto dir=batchdir::parse(news[b],news[b].size());sum+=batchdir::value(dir,news[b],n)->size();}else{auto h=mode==3?batchdir::header(news[b],news[b].size()):batchdir::header_from_checked_pages(news[b],news[b].size());sum+=batchdir::value(batchdir::entry(h,news[b],n),news[b])->size();}}access[mode]=std::chrono::duration<double,std::milli>(Clock::now()-a).count();}
 std::cout<<"{\"corpus\":"<<corpus<<",\"varchar\":"<<varchar<<",\"round\":"<<round<<",\"old_encode_ms\":"<<old_encode<<",\"new_encode_ms\":"<<new_encode<<",\"old_estimated_file_bytes\":"<<old_bytes<<",\"new_estimated_file_bytes\":"<<new_bytes<<",\"old_random_ms\":"<<access[0]<<",\"cached_directory_random_ms\":"<<access[1]<<",\"uncached_directory_random_ms\":"<<access[2]<<",\"checked_header_random_ms\":"<<access[3]<<",\"frame_verified_point_random_ms\":"<<access[4]<<",\"digest\":"<<sum<<"}\n";
 }
 }
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
