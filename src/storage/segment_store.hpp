#pragma once
#include "minivectordb/store.hpp"
#include "page_checksum.hpp"
#include "../vendor/lz4/lz4.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <system_error>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace minivectordb::detail {
// J09 keeps immutable, independently decoded mutation batches. Savepoints name
// a complete batch head; row locations encode that head plus a seven-bit slot.
// No dictionary dependency on preceding batches. J08 remains separately readable.
class SegmentStore {
  using Key=std::pair<std::uint64_t,std::int64_t>;
  using Page=std::array<unsigned char,4096>;
  struct Row {Key key;std::size_t offset,length;bool deleted;};
  struct Decoded {std::string bytes;std::vector<Row> rows;std::uint64_t previous=0;};
  struct Cached {std::uint64_t at=0,age=0;std::shared_ptr<const Decoded> value;};
  struct Undo {Key key;std::uint64_t value;};
  struct Group {std::uint64_t head;std::size_t begin;};
  int fd_;
  CommitSyncPolicy policy_;
  std::uint64_t head_=0,durable_=0,end_=8192,generation_=0;
  std::map<Key,std::uint64_t> index_;
  std::vector<Undo> undo_;
  std::vector<Group> groups_;
  std::size_t empty_=0,budget_=0;
  mutable std::vector<Cached> cache_;
  mutable std::size_t resident_=0;
  mutable std::uint64_t clock_=0;
  static constexpr std::size_t header_size=48;
#ifndef MVDB_SEGMENT_TARGET
#define MVDB_SEGMENT_TARGET 4096
#endif
  static constexpr std::size_t segment_target=MVDB_SEGMENT_TARGET;
  static constexpr std::size_t compressed_limit=Store::max_batch_bytes+24*Store::max_batch_mutations;
  static void fail(){throw std::runtime_error("corrupt MiniVectorDB segment");}
  static void io(const char* what){throw std::system_error(errno,std::generic_category(),what);}
  static std::uint64_t get(const unsigned char* p,unsigned n=8){std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(p[i])<<(8*i);return v;}
  static void put(unsigned char* p,std::uint64_t v,unsigned n=8){for(unsigned i=0;i<n;++i)p[i]=static_cast<unsigned char>(v>>(8*i));}
  static std::uint32_t crc(std::string_view s){return ~page_crc32_span(0xffffffffU,reinterpret_cast<const unsigned char*>(s.data()),s.size());}
  static std::size_t charge(const Decoded& d){return sizeof(Decoded)+d.bytes.capacity()+1+d.rows.capacity()*sizeof(Row);}
  void sync(){for(;;){profile::detail::Scope timer(&profile::Snapshot::fsync);if(::fsync(fd_)==0)return;if(errno!=EINTR)io("segment fsync");}}
  void write(std::uint64_t at,std::string_view data){
    if(at>std::uint64_t(std::numeric_limits<off_t>::max())||data.size()>std::uint64_t(std::numeric_limits<off_t>::max())-at)throw std::length_error("segment offset exhausted");
    std::size_t done=0;
    while(done<data.size()) {ssize_t n;{profile::detail::Scope timer(&profile::Snapshot::pwrite);n=::pwrite(fd_,data.data()+done,data.size()-done,off_t(at+done));if(n>0)timer.bytes(std::uint64_t(n));}
      if(n<0){if(errno==EINTR)continue;io("segment pwrite");}if(!n)throw std::runtime_error("zero segment pwrite");done+=std::size_t(n);
    }
  }
  void read(std::uint64_t at,char* out,std::size_t size)const{
    if(at>end_||size>end_-at)fail();std::size_t done=0;
    while(done<size){ssize_t n;{profile::detail::Scope timer(&profile::Snapshot::pread);n=::pread(fd_,out+done,size-done,off_t(at+done));if(n>0)timer.bytes(std::uint64_t(n));}
      if(n<0){if(errno==EINTR)continue;io("segment pread");}if(!n)fail();done+=std::size_t(n);
    }
  }
  std::shared_ptr<const Decoded> block(std::uint64_t at,std::uint64_t extent)const{
    for(auto& c:cache_)if(c.at==at&&c.value){c.age=++clock_;return c.value;}
    if(at<8192||at>extent||header_size>extent-at)fail();
    std::array<unsigned char,header_size> h{};read(at,reinterpret_cast<char*>(h.data()),h.size());
    if(std::memcmp(h.data(),"JB09",4)||get(h.data()+44,4)!=crc({reinterpret_cast<const char*>(h.data()),44}))fail();
    const auto flags=get(h.data()+4,4),previous=get(h.data()+8),raw=get(h.data()+16),packed=get(h.data()+24),count=get(h.data()+32,4);
    if(flags>2||!count||count>128||raw<count*24||packed>extent-at-header_size||(previous&&(previous<8192||previous>=at)))fail();
    if(flags&&(raw>compressed_limit||packed>=raw))fail();
    if(!flags&&raw!=packed)fail();
    if(raw>std::string().max_size()||packed>std::string().max_size())fail();
    auto result=std::make_shared<Decoded>();result->previous=previous;
    std::string encoded(std::size_t(packed),'\0');read(at+header_size,encoded.data(),encoded.size());
    if(crc(encoded)!=get(h.data()+36,4))fail();
    if(!flags)result->bytes=std::move(encoded);
    else {
      std::string decoded(std::size_t(raw),'\0');
      if(LZ4_decompress_safe(encoded.data(),decoded.data(),int(packed),int(raw))!=int(raw))fail();
      if(flags==1)result->bytes=std::move(decoded);
      else {result->bytes.resize(std::size_t(raw));std::size_t off=0;for(std::size_t lane=0;lane<4;++lane)for(std::size_t i=lane;i<raw;i+=4)result->bytes[i]=decoded[off++];}
    }
    if(crc(result->bytes)!=get(h.data()+40,4))fail();
    result->rows.reserve(std::size_t(count));std::size_t off=0;
    for(std::size_t i=0;i<count;++i){if(result->bytes.size()-off<24)fail();const auto* p=reinterpret_cast<const unsigned char*>(result->bytes.data()+off);
      const Key key{get(p),std::bit_cast<std::int64_t>(get(p+8))};const auto length=get(p+16);off+=24;const bool deleted=length==UINT64_MAX;
      if(!deleted&&length>result->bytes.size()-off)fail();
      result->rows.push_back({key,off,deleted?0:std::size_t(length),deleted});if(!deleted)off+=std::size_t(length);
      if(i&&!(result->rows[i-1].key<key))fail();
    }
    if(off!=result->bytes.size())fail();
    const auto size=charge(*result);
    if(!cache_.empty()&&size<=budget_) {
      while(resident_>budget_-size){auto it=std::min_element(cache_.begin(),cache_.end(),[](const auto&a,const auto&b){return (a.value?a.age:UINT64_MAX)<(b.value?b.age:UINT64_MAX);});if(!it->value)break;resident_-=charge(*it->value);*it={};}
      auto it=std::min_element(cache_.begin(),cache_.end(),[](const auto&a,const auto&b){return (a.value?a.age:0)<(b.value?b.age:0);});
      if(it->value)resident_-=charge(*it->value);*it={at,++clock_,result};resident_+=size;
    }
    return result;
  }
  static std::uint64_t location(std::uint64_t at,std::size_t slot){if(at>UINT64_MAX/128||slot>=128)throw std::length_error("segment location exhausted");return at*128+slot;}
  auto reconstruct(std::uint64_t head,std::uint64_t extent)const{
    // Walk newest first; first sighting wins (including tombstones).
    std::map<Key,std::uint64_t> result;
    for(auto at=head;at;){auto b=block(at,extent);for(std::size_t i=0;i<b->rows.size();++i){const auto&r=b->rows[i];result.try_emplace(r.key,r.deleted?0:location(at,i));}at=b->previous;}
    for(auto i=result.begin();i!=result.end();)if(!i->second)i=result.erase(i);else ++i;
    return result;
  }
  Page root(std::uint64_t gen)const{Page p{};std::memcpy(p.data(),"MVDBSG09",8);put(p.data()+8,gen);put(p.data()+16,head_);put(p.data()+24,end_);put(p.data()+32,page_checksum(p,32),4);return p;}
  static bool valid(const Page&p,std::uint64_t extent){if(!magic(p.data())||get(p.data()+32,4)!=page_checksum(p,32))return false;const auto head=get(p.data()+16),end=get(p.data()+24);return end>=8192&&end<=extent&&(!head||(head>=8192&&head<=end&&header_size<=end-head));}
  void assign(std::uint64_t& slot,std::uint64_t value){if(!slot&&value)--empty_;else if(slot&&!value)++empty_;slot=value;}
  void purge(){if(!empty_)return;for(auto i=index_.begin();i!=index_.end();)if(!i->second){i=index_.erase(i);--empty_;}else ++i;}
public:
  static bool magic(const unsigned char*p){return std::memcmp(p,"MVDBSG09",8)==0;}
  SegmentStore(int fd,bool created,std::size_t budget,CommitSyncPolicy policy):fd_(fd),policy_(policy),budget_(std::min(budget,std::size_t(512*1024))){
    cache_.resize(std::min(std::size_t(128),budget_/(4096+sizeof(Cached))));budget_-=cache_.capacity()*sizeof(Cached);
    if(created){Page zero{};write(4096,{reinterpret_cast<const char*>(zero.data()),zero.size()});auto p=root(0);write(0,{reinterpret_cast<const char*>(p.data()),p.size()});sync();return;}
    struct stat st{};if(::fstat(fd_,&st))io("segment fstat");if(st.st_size<8192)fail();end_=std::uint64_t(st.st_size);
    Page a{},b{};read(0,reinterpret_cast<char*>(a.data()),a.size());read(4096,reinterpret_cast<char*>(b.data()),b.size());const bool av=valid(a,end_),bv=valid(b,end_);if(!av&&!bv)fail();
    const auto& p=!av||(bv&&get(b.data()+8)>get(a.data()+8))?b:a;generation_=get(p.data()+8);head_=durable_=get(p.data()+16);index_=reconstruct(head_,get(p.data()+24));
  }
  std::uint64_t head()const noexcept{return head_;}
  void begin(){undo_.clear();groups_.clear();}
  std::optional<std::string> value(std::uint64_t space,std::int64_t key)const{auto it=index_.find({space,key});if(it==index_.end()||!it->second)return {};return value_at(space,key,it->second);}
  std::string value_at(std::uint64_t space,std::int64_t key,std::uint64_t pos)const{auto b=block(pos/128,end_);const auto slot=std::size_t(pos%128);if(slot>=b->rows.size())fail();const auto&r=b->rows[slot];if(r.deleted||r.key!=Key{space,key})fail();return b->bytes.substr(r.offset,r.length);}
  void apply(std::uint64_t space,std::span<const Store::Mutation> changes){
    if(changes.size()>128)throw std::length_error("too many batch mutations");
    std::vector<const Store::Mutation*> order;order.reserve(changes.size());std::size_t size=0;
    for(const auto&c:changes){if(c.value&&changes.size()>1){if(c.value->size()>Store::max_batch_bytes-size)throw std::length_error("batch payload exceeds byte budget");size+=c.value->size();}order.push_back(&c);}
    std::sort(order.begin(),order.end(),[](auto a,auto b){return a->key<b->key;});for(std::size_t i=1;i<order.size();++i)if(order[i-1]->key==order[i]->key)throw std::invalid_argument("duplicate batch key");if(order.empty())return;
    std::string encoded;std::vector<std::uint64_t> locations;locations.reserve(order.size());auto previous=head_;
    for(std::size_t begin=0;begin<order.size();) {
      std::size_t finish=begin,estimate=0;
      while(finish<order.size()) {
        const auto row_size=24+(order[finish]->value?order[finish]->value->size():0);
        if(finish>begin&&(row_size>segment_target||estimate>segment_target-row_size))break;
        if(row_size>std::string().max_size()-estimate)throw std::length_error("segment payload too large");
        estimate+=row_size;++finish;
      }
      std::string raw;raw.reserve(estimate);
      for(auto i=begin;i<finish;++i){const auto*c=order[i];std::array<unsigned char,24> r{};put(r.data(),space);put(r.data()+8,std::bit_cast<std::uint64_t>(c->key));put(r.data()+16,c->value?c->value->size():UINT64_MAX);raw.append(reinterpret_cast<const char*>(r.data()),r.size());if(c->value)raw.append(*c->value);}
      std::string compressed;unsigned flags=0;std::string_view payload=raw;
      if(raw.size()>=64&&raw.size()<=compressed_limit){
        std::string planes(raw.size(),'\0');std::size_t off=0;for(std::size_t lane=0;lane<4;++lane)for(std::size_t i=lane;i<raw.size();i+=4)planes[off++]=raw[i];
        compressed.resize(std::size_t(LZ4_compressBound(int(raw.size()))));
        const auto n=LZ4_compress_default(planes.data(),compressed.data(),int(raw.size()),int(compressed.size()));
        if(n>0&&std::size_t(n)+8<raw.size()){compressed.resize(std::size_t(n));payload=compressed;flags=2;}
      }
      if(encoded.size()>UINT64_MAX-end_)throw std::length_error("segment extent exhausted");
      const auto at=end_+encoded.size();
      for(auto i=begin;i<finish;++i)locations.push_back(location(at,i-begin));
      std::array<unsigned char,header_size> h{};std::memcpy(h.data(),"JB09",4);put(h.data()+4,flags,4);put(h.data()+8,previous);put(h.data()+16,raw.size());put(h.data()+24,payload.size());put(h.data()+32,finish-begin,4);put(h.data()+36,crc(payload),4);put(h.data()+40,crc(raw),4);put(h.data()+44,crc({reinterpret_cast<const char*>(h.data()),44}),4);
      encoded.append(reinterpret_cast<const char*>(h.data()),h.size());encoded.append(payload);previous=at;begin=finish;
    }
    if(encoded.size()>UINT64_MAX-end_)throw std::length_error("segment extent exhausted");
    undo_.reserve(undo_.size()+order.size());groups_.reserve(groups_.size()+1);
    for(const auto*c:order)if(index_.try_emplace(Key{space,c->key},0).second)++empty_;
    write(end_,encoded);end_+=encoded.size();
    groups_.push_back({head_,undo_.size()});
    for(std::size_t i=0;i<order.size();++i){const Key key{space,order[i]->key};auto& slot=index_.find(key)->second;undo_.push_back({key,slot});assign(slot,order[i]->value?locations[i]:0);}
    head_=previous;
  }

  void restore(std::uint64_t target){
    if(target==head_)return;
    if(!std::any_of(groups_.begin(),groups_.end(),[&](const auto&g){return g.head==target;})){auto rebuilt=reconstruct(target,end_);index_.swap(rebuilt);empty_=0;head_=target;undo_.clear();groups_.clear();return;}
    while(head_!=target){const auto g=groups_.back();groups_.pop_back();while(undo_.size()>g.begin){const auto u=undo_.back();undo_.pop_back();assign(index_.find(u.key)->second,u.value);}head_=g.head;}
  }
  void rollback(){restore(durable_);undo_.clear();groups_.clear();purge();}
  void commit(){if(generation_==UINT64_MAX)throw std::overflow_error("generation exhausted");const auto next=generation_+1;if(policy_==CommitSyncPolicy::Ordered)sync();auto p=root(next);write((next%2)*4096,{reinterpret_cast<const char*>(p.data()),p.size()});sync();generation_=next;durable_=head_;undo_.clear();groups_.clear();purge();}
  auto snapshot(std::uint64_t space)const{std::vector<std::pair<std::int64_t,std::uint64_t>> rows;for(auto i=index_.lower_bound({space,INT64_MIN});i!=index_.end()&&i->first.first==space;++i)if(i->second)rows.push_back({i->first.second,i->second});return rows;}
};
}
