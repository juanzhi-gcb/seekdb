#pragma once
#include "minivectordb/store.hpp"
#include "packed_codec.hpp"
#include "batch_directory.hpp"
#include "page_checksum.hpp"
#include <map>
#include <vector>
#include <limits>
#include <bit>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

namespace minivectordb::detail {
// J08/J10 remain readable and writable. New J11 files share batch metadata
// while keeping each row independently compressed. Framed files reserve a 48-byte footer
// per 4096-byte frame; a commit shares the final data frame, eliminating the
// separate mutable root-page write. Record offsets address 4048-byte payload
// regions. Footer and payload checksums are separate so torn roots and corrupt
// committed data are distinguishable. Rollback changes reachability, never truncates a
// record that an in-flight scan or savepoint may still reference. The in-memory
// ordered directory stores offsets only; no second copy of row payloads.
class JournalStore {
  using Key=std::pair<std::uint64_t,std::int64_t>;
  using Page=std::array<unsigned char,4096>;
  struct Undo {Key key;std::uint64_t value,head;};
  struct Cached {Page bytes{};std::uint64_t page=UINT64_MAX;std::size_t size=0;};
  int fd_;
  CommitSyncPolicy policy_;
  bool framed_=false,batches_=false,pending_valid_=false;
  // A tagged reference contains a virtual batch offset, row slot, count and
  // offset-width hint. The hints allow one bounded metadata read on a miss.
  // Tags never appear in J08/J10 files; ordinary JREC offsets remain unchanged.
  static constexpr std::uint64_t batch_tag=std::uint64_t(1)<<63;
  // Row references exist only in the validated in-memory index. Head/previous
  // references on disk always use ordinary JREC or batch-directory tokens.
  static constexpr std::uint64_t row_tag=batch_tag|(1ULL<<62),row_mask=(1ULL<<41)-1;
  static bool direct_row(std::uint64_t at){return (at&row_tag)==row_tag;}
  static std::uint64_t row_token(std::uint64_t payload,unsigned length,bool compressed){
    if(payload>row_mask || length>Store::max_batch_bytes)throw std::length_error("direct journal row reference");
    return row_tag|(std::uint64_t(compressed)<<61)|(std::uint64_t(length)<<41)|payload;
  }
  static bool tagged(std::uint64_t at){return (at&batch_tag)!=0;}
  static std::uint64_t address(std::uint64_t at){return direct_row(at)?(at&row_mask):(tagged(at)?((at&~batch_tag)>>15):at);}
  static std::uint64_t token(std::uint64_t at,unsigned slot,unsigned count,unsigned width){return batch_tag|(at<<15)|((std::uint64_t(width==4))<<14)|(std::uint64_t(count-1)<<7)|slot;}
  static unsigned token_count(std::uint64_t at){return unsigned((at>>7)&127)+1;}
  static unsigned token_width(std::uint64_t at){return (at&(1ULL<<14))?4:2;}
  static bool precedes(std::uint64_t a,std::uint64_t b){
    return address(a)<address(b)||(address(a)==address(b)&&tagged(a)&&tagged(b)&&(a&127)<(b&127));
  }
  struct BatchCache {std::uint64_t at=0;batchdir::Header header;std::string prefix;bool fully_validated=false;};
  mutable std::array<BatchCache,2> batch_cache_{};
  mutable unsigned batch_next_=0;
  static constexpr std::size_t frame_payload=4048;
  Page pending_{};
  std::uint64_t pending_block_=0;
  std::size_t pending_used_=0;
  std::uint64_t head_=0,durable_=0,end_=8192,generation_=0;
  std::map<Key,std::uint64_t> index_;
  std::size_t empty_entries_=0;
  std::vector<Undo> undo_;
  mutable std::vector<Cached> cache_;
  static constexpr std::size_t header_size=48;
  static void fail(){throw std::runtime_error("corrupt MiniVectorDB journal");}
  static void io(const char* what){throw std::system_error(errno,std::generic_category(),what);}
  static std::uint64_t get(const unsigned char* p,unsigned n=8){std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(p[i])<<(8*i);return v;}
  static void put(unsigned char* p,std::uint64_t v,unsigned n=8){for(unsigned i=0;i<n;++i)p[i]=static_cast<unsigned char>(v>>(8*i));}
  static std::uint32_t crc(std::string_view s){return ~page_crc32_span(0xffffffffU,reinterpret_cast<const unsigned char*>(s.data()),s.size());}
  void sync(){for(;;){profile::detail::Scope timer(&profile::Snapshot::fsync);if(::fsync(fd_)==0)return;if(errno!=EINTR)io("journal fsync");}}
  void write_raw(std::uint64_t offset,std::string_view bytes){
    if(offset>std::uint64_t(std::numeric_limits<off_t>::max()) || bytes.size()>std::uint64_t(std::numeric_limits<off_t>::max())-offset)throw std::length_error("journal offset exhausted");
    if(!framed_&&!bytes.empty())for(auto& c:cache_)if(c.page>=offset/4096 && c.page<=(offset+bytes.size()-1)/4096)c.page=UINT64_MAX;
    std::size_t done=0;
    while(done<bytes.size()){
      ssize_t n;{profile::detail::Scope timer(&profile::Snapshot::pwrite);n=::pwrite(fd_,bytes.data()+done,bytes.size()-done,off_t(offset+done));if(n>0)timer.bytes(std::uint64_t(n));}
      if(n<0){if(errno==EINTR)continue;io("journal pwrite");}if(!n)throw std::runtime_error("zero-length journal pwrite");done+=std::size_t(n);
    }
  }
  std::size_t read_some(std::uint64_t offset,unsigned char* output,std::size_t size)const{
    std::size_t done=0;
    while(done<size){ssize_t n;{profile::detail::Scope timer(&profile::Snapshot::pread);n=::pread(fd_,output+done,size-done,off_t(offset+done));if(n>0)timer.bytes(std::uint64_t(n));}
      if(n<0){if(errno==EINTR)continue;io("journal pread");}if(!n)break;done+=std::size_t(n);
    }return done;
  }
  static std::uint64_t physical(std::uint64_t block){
    const auto max=std::uint64_t(std::numeric_limits<off_t>::max());
    if(block>(max-8192)/4096)throw std::length_error("framed journal offset exhausted");
    return 8192+block*4096;
  }
  bool frame_header(const Page& p) const {
    const auto* f=p.data()+frame_payload;
    return !std::memcmp(f,batches_?"JFR11":"JFR10",5) && f[5]<=1 && !f[6] && !f[7] &&
      get(f+32,4)<=frame_payload && get(f+40,4)==crc({reinterpret_cast<const char*>(f),40});
  }
  static bool frame_payload_valid(const Page& p){return get(p.data()+frame_payload+36,4)==crc({reinterpret_cast<const char*>(p.data()),frame_payload});}
  void invalidate_block(std::uint64_t block){for(auto& c:cache_)if(c.page==block)c.page=UINT64_MAX;}
  Page frame(bool commit,std::uint64_t generation) const {
    Page p=pending_valid_?pending_:Page{};
    auto* f=p.data()+frame_payload;std::memset(f,0,48);std::memcpy(f,batches_?"JFR11":"JFR10",5);f[5]=commit?1:0;
    if(commit){put(f+8,generation);put(f+16,head_);put(f+24,8192+(pending_block_+1)*frame_payload);}
    put(f+32,pending_used_,4);put(f+36,crc({reinterpret_cast<const char*>(p.data()),frame_payload}),4);
    put(f+40,crc({reinterpret_cast<const char*>(f),40}),4);return p;
  }
  void flush_frame(bool commit,std::uint64_t generation,bool footer_only=false){
    const auto p=frame(commit,generation);invalidate_block(pending_block_);
    const auto first=footer_only?frame_payload:0;
    write_raw(physical(pending_block_)+first,{reinterpret_cast<const char*>(p.data()+first),p.size()-first});
  }
  void write(std::uint64_t offset,std::string_view bytes){
    if(!framed_ || offset<8192){write_raw(offset,bytes);return;}
    if(offset!=end_ || offset>std::uint64_t(std::numeric_limits<off_t>::max()) || bytes.size()>std::uint64_t(std::numeric_limits<off_t>::max())-offset)throw std::length_error("framed append position");
    // A bounded preimage keeps an earlier statement readable after any short
    // write or I/O error while appending this batch. Published offsets change
    // only after the caller has written the complete batch successfully.
    const auto old=pending_;const auto old_block=pending_block_;const auto old_used=pending_used_;const bool old_valid=pending_valid_;
    // Coalesce completed frames into bounded sequential writes. The final
    // frame remains pending for its commit footer; no barrier is removed.
    std::array<unsigned char,64*1024> staged;
    std::size_t staged_size=0;std::uint64_t staged_at=0;
    const auto flush_staged=[&]{if(staged_size){write_raw(staged_at,{reinterpret_cast<const char*>(staged.data()),staged_size});staged_size=0;}};
    try {
      std::size_t done=0;
      while(done<bytes.size()){
        const auto position=offset+done,block=(position-8192)/frame_payload;
        const auto inside=std::size_t((position-8192)%frame_payload);
        if(pending_valid_ && pending_block_!=block){
          if(pending_used_!=frame_payload)throw std::logic_error("framed append gap");
          const auto p=frame(false,0);invalidate_block(pending_block_);
          if(!staged_size)staged_at=physical(pending_block_);
          std::memcpy(staged.data()+staged_size,p.data(),p.size());staged_size+=p.size();
          if(staged_size==staged.size())flush_staged();pending_valid_=false;
        }
        if(!pending_valid_){if(inside)throw std::logic_error("framed append prefix");pending_={};pending_block_=block;pending_used_=0;pending_valid_=true;}
        if(inside!=pending_used_)throw std::logic_error("framed append overlap");
        const auto n=std::min(bytes.size()-done,frame_payload-inside);
        std::memcpy(pending_.data()+inside,bytes.data()+done,n);pending_used_+=n;done+=n;invalidate_block(block);
      }
      flush_staged();
    }catch(...){pending_=old;pending_block_=old_block;pending_used_=old_used;pending_valid_=old_valid;for(auto& c:cache_)c.page=UINT64_MAX;throw;}
  }
  std::string read_framed(std::uint64_t offset,std::size_t size)const{
    if(offset<8192 || offset>end_ || size>end_-offset)fail();std::string out(size,'\0');std::size_t done=0;
    while(done<size){
      const auto at=offset+done,block=(at-8192)/frame_payload;const auto inside=std::size_t((at-8192)%frame_payload);
      const auto n=std::min(size-done,frame_payload-inside);
      if(pending_valid_ && block==pending_block_){if(inside+n>pending_used_)fail();std::memcpy(out.data()+done,pending_.data()+inside,n);}
      else {
        // On a cache hit no temporary page is read at all. On a miss,
        // read_some must fill all 4096 bytes before either checksum is tested;
        // the short-circuit size guard rejects partial reads first.
        // Do not clear 4 KiB on every cached header/payload lookup.
        Page scratch;const Page* page=nullptr;
        if(cache_.empty()){if(read_some(physical(block),scratch.data(),scratch.size())!=scratch.size() || !frame_header(scratch) || !frame_payload_valid(scratch))fail();page=&scratch;}
        else {auto& entry=cache_[block%cache_.size()];
          if(entry.page!=block){entry.page=UINT64_MAX;if(read_some(physical(block),entry.bytes.data(),entry.bytes.size())!=entry.bytes.size() || !frame_header(entry.bytes) || !frame_payload_valid(entry.bytes))fail();entry.page=block;}
          page=&entry.bytes;
        }
        if(inside+n>get(page->data()+frame_payload+32,4))fail();std::memcpy(out.data()+done,page->data()+inside,n);
      }
      done+=n;
    }
    return out;
  }
  std::string read(std::uint64_t offset,std::size_t size)const{
    if(framed_)return read_framed(offset,size);
    if(offset>end_ || size>end_-offset)fail();std::string out(size,'\0');std::size_t done=0;
    if(cache_.empty()){if(read_some(offset,reinterpret_cast<unsigned char*>(out.data()),size)!=size)fail();return out;}
    while(done<size){const auto at=offset+done,page=at/4096;auto& entry=cache_[page%cache_.size()];
      if(entry.page!=page){entry.page=UINT64_MAX;entry.size=read_some(page*4096,entry.bytes.data(),4096);entry.page=page;}
      const auto inside=std::size_t(at%4096),n=std::min(size-done,4096-inside);if(inside+n>entry.size)fail();
      std::memcpy(out.data()+done,entry.bytes.data()+inside,n);done+=n;
    }return out;
  }
  struct Record {Key key;std::uint64_t previous,length,payload;std::uint32_t flags,checksum;};
  Record record(std::uint64_t at,std::uint64_t extent,bool recovery=false)const{
    const auto base=address(at);
    if(direct_row(at) || base<8192 || base>extent || header_size>extent-base || (tagged(at)&&!batches_))fail();
    if(tagged(at)){
      BatchCache* c=nullptr;
      for(auto& entry:batch_cache_)if(entry.at==base){c=&entry;break;}
      if(!c){
        const auto count=token_count(at),width=token_width(at);
        if(count<2 || (at&127)>=count)fail();
        // Count/width are only read-size hints. Validate against the CRC-protected
        // header before interpreting descriptors, even for a cached reference.
        const auto prefix_size=batchdir::header_size+count*(2+width);
        if(prefix_size>extent-base)fail();
        auto prefix=read(base,prefix_size);const auto h=batchdir::header_from_checked_pages(prefix,extent-base);
        if(count!=h.count || width!=h.width)fail();
        auto& target=batch_cache_[batch_next_++%batch_cache_.size()];target.at=0;
        target.header=h;target.prefix=std::move(prefix);target.fully_validated=false;target.at=base;c=&target;
      }
      const auto& h=c->header;
      if(h.bytes>extent-base || token_count(at)!=h.count || token_width(at)!=h.width || (at&127)>=h.count)fail();
      // Recovery validates every descriptor once per batch. Normal point reads
      // validate the needed slot only; read() has already checked every frame.
      if(recovery && !c->fully_validated){(void)batchdir::parse(c->prefix,extent-base);c->fully_validated=true;}
      const auto slot=unsigned(at&127);const auto e=batchdir::entry(h,c->prefix,slot);
      const auto previous=slot?token(base,slot-1,h.count,h.width):h.previous;
      if(previous && (direct_row(previous) || address(previous)<8192 || !precedes(previous,at)))fail();
      return {{h.space,e.key},previous,e.length,base+e.offset,e.deleted?2U:(e.compressed?1U:0U),0};
    }
    const auto bytes=read(at,header_size);const auto* p=reinterpret_cast<const unsigned char*>(bytes.data());
    if(std::memcmp(p,"JREC",4) || get(p+44,4)!=crc(std::string_view(bytes).substr(0,44)))fail();
    Record r{{get(p+8),std::bit_cast<std::int64_t>(get(p+16))},get(p+24),get(p+32),at+header_size,std::uint32_t(get(p+4,4)),std::uint32_t(get(p+40,4))};
    if(r.flags>2 || (r.previous && (direct_row(r.previous) || address(r.previous)<8192 || !precedes(r.previous,at) || (tagged(r.previous)&&!batches_))) || r.length>extent-at-header_size || (r.flags==2 && (r.length||r.checksum)))fail();return r;
  }
  auto reconstruct(std::uint64_t head,std::uint64_t extent)const{
    std::vector<std::uint64_t> positions;
    for(auto at=head;at;){positions.push_back(at);at=record(at,extent,true).previous;}
    std::map<Key,std::uint64_t> result;
    for(auto i=positions.rbegin();i!=positions.rend();++i){auto r=record(*i,extent,true);if(r.flags==2)result.erase(r.key);else result[r.key]=tagged(*i)?row_token(r.payload-4,unsigned(r.length),r.flags==1):*i;}
    return result;
  }
  Page root(std::uint64_t generation)const{
    Page p{};std::memcpy(p.data(),batches_?"MVDBJL11":(framed_?"MVDBJL10":"MVDBJL08"),8);put(p.data()+8,generation);put(p.data()+16,head_);put(p.data()+24,end_);put(p.data()+32,page_checksum(p,32),4);return p;
  }
  static bool valid(const Page&p,std::uint64_t bytes){
    if((std::memcmp(p.data(),"MVDBJL08",8) && std::memcmp(p.data(),"MVDBJL10",8) && std::memcmp(p.data(),"MVDBJL11",8)) || get(p.data()+32,4)!=page_checksum(p,32))return false;
    if(!std::memcmp(p.data(),"MVDBJL10",8)||!std::memcmp(p.data(),"MVDBJL11",8))return !get(p.data()+8)&&!get(p.data()+16)&&get(p.data()+24)==8192;
    const auto head=get(p.data()+16),end=get(p.data()+24);return end>=8192 && end<=bytes && (!head || (head>=8192 && head<=end && header_size<=end-head));
  }
  void assign(std::uint64_t& slot,std::uint64_t value){if(!slot&&value)--empty_entries_;else if(slot&&!value)++empty_entries_;slot=value;}
  void purge_empty(){if(!empty_entries_)return;for(auto i=index_.begin();i!=index_.end();)if(!i->second){i=index_.erase(i);--empty_entries_;}else ++i;}
public:
  static bool magic(const unsigned char* p){return std::memcmp(p,"MVDBJL08",8)==0 || std::memcmp(p,"MVDBJL10",8)==0 || std::memcmp(p,"MVDBJL11",8)==0;}
  JournalStore(int fd,bool created,std::size_t cache_budget,CommitSyncPolicy policy):fd_(fd),policy_(policy){
    cache_.resize(std::min(cache_budget,std::size_t(512*1024))/sizeof(Cached));
    if(created){framed_=batches_=true;auto p=root(0);write_raw(4096,{reinterpret_cast<const char*>(p.data()),p.size()});write_raw(0,{reinterpret_cast<const char*>(p.data()),p.size()});sync();return;}
    struct stat stat{};if(::fstat(fd_,&stat))io("journal fstat");if(stat.st_size<8192)fail();end_=std::uint64_t(stat.st_size);
    Page a{},b{};if(read_some(0,a.data(),a.size())!=a.size()||read_some(4096,b.data(),b.size())!=b.size())fail();
    const bool av=valid(a,end_),bv=valid(b,end_);if(!av&&!bv)fail();const auto& p=!av||(bv&&get(b.data()+8)>get(a.data()+8))?b:a;
    if(av&&bv&&std::memcmp(a.data(),b.data(),8))fail();
    batches_=!std::memcmp(p.data(),"MVDBJL11",8);framed_=batches_||!std::memcmp(p.data(),"MVDBJL10",8);
    if(!framed_){generation_=get(p.data()+8);head_=durable_=get(p.data()+16);index_=reconstruct(head_,get(p.data()+24));return;}
    const auto complete=(end_-8192)/4096;std::uint64_t committed_pages=0;end_=8192;
    for(auto count=complete;count;--count){Page page{};
      if(read_some(physical(count-1),page.data(),page.size())!=page.size())fail();
      if(!frame_header(page) || page[frame_payload+5]!=1)continue;
      const auto* f=page.data()+frame_payload;const auto extent=get(f+24),head=get(f+16),generation=get(f+8);
      if(!generation || extent!=8192+count*frame_payload || (head&&(direct_row(head)||address(head)<8192||address(head)>=extent||(tagged(head)&&!batches_))))fail();
      // A valid commit header with bad data is corruption, not an incomplete
      // footer. Never silently return an older generation for damaged payloads.
      if(!frame_payload_valid(page))fail();
      generation_=generation;head_=durable_=head;end_=extent;committed_pages=count;break;
    }
    index_=reconstruct(head_,end_);
    // No in-flight snapshots survive reopening. Discard only the uncommitted
    // physical suffix, including torn frames, before accepting another append.
    const auto committed_size=physical(committed_pages);
    if(committed_size!=std::uint64_t(stat.st_size) && ::ftruncate(fd_,off_t(committed_size)))io("framed journal truncate orphan tail");
  }
  std::uint64_t head()const noexcept{return head_;}
  void begin(){undo_.clear();}
  std::optional<std::string> value(std::uint64_t space,std::int64_t key)const{
    const auto it=index_.find({space,key});if(it==index_.end()||!it->second)return {};return value_at(space,key,it->second);
  }
  std::string value_at(std::uint64_t space,std::int64_t key,std::uint64_t at)const{
    if(direct_row(at)){
      // The namespace/key association was validated when publishing/rebuilding
      // the index. This opaque reference is never accepted from disk or users.
      const auto length=std::size_t((at>>41)&((1ULL<<20)-1));
      if(length>Store::max_batch_bytes)fail();const auto bytes=read(address(at),length+4);
      const auto payload=std::string_view(bytes).substr(4);
      if(crc(payload)!=get(reinterpret_cast<const unsigned char*>(bytes.data()),4))fail();
      return (at&(1ULL<<61))?unpack_bytes(payload,4064,true):std::string(payload);
    }
    const auto r=record(at,end_);if(r.key!=Key{space,key}||r.flags==2)fail();auto payload=read(r.payload,std::size_t(r.length));
    if(crc(payload)!=r.checksum)fail();return r.flags==1?unpack_bytes(payload,4064,true):payload;
  }
  void apply(std::uint64_t space,std::span<const Store::Mutation> changes){
    if(changes.size()>Store::max_batch_mutations)throw std::length_error("too many batch mutations");
    std::vector<const Store::Mutation*> order;order.reserve(changes.size());std::size_t bytes=0;
    for(const auto& c:changes){if(c.value&&changes.size()>1){if(c.value->size()>Store::max_batch_bytes-bytes)throw std::length_error("batch payload exceeds byte budget");bytes+=c.value->size();}order.push_back(&c);}
    std::sort(order.begin(),order.end(),[](auto a,auto b){return a->key<b->key;});for(std::size_t i=1;i<order.size();++i)if(order[i-1]->key==order[i]->key)throw std::invalid_argument("duplicate batch key");
    if(order.empty())return;
    std::string encoded,packed;struct Update{Key key;std::uint64_t value,head;};std::vector<Update> updates;updates.reserve(order.size());auto previous=head_;
    // The token representation is bounded; fall back to ordinary records for
    // wide key ranges, oversized values, single rows or very large journals.
    const bool compact=batches_ && end_<row_mask-(Store::max_batch_bytes+4096) && batchdir::encode(space,previous,order,encoded);
    if(compact){const auto h=batchdir::header(encoded,encoded.size());for(unsigned i=0;i<order.size();++i){
      const auto e=batchdir::entry(h,encoded,i);const auto at=token(end_,i,h.count,h.width);
      const auto value=e.deleted?0:row_token(end_+e.offset-4,e.length,e.compressed);
      updates.push_back({{space,order[i]->key},value,at});previous=at;}}
    else for(const auto* c:order){std::string_view payload=c->value?std::string_view(*c->value):std::string_view{};unsigned flags=c->value?0:2;
      if(c->value&&pack_bytes(payload,packed,true,true)){payload=packed;flags=1;}
      if(encoded.size()>UINT64_MAX-end_)throw std::length_error("journal size");const auto at=end_+encoded.size();std::array<unsigned char,header_size> h{};
      std::memcpy(h.data(),"JREC",4);put(h.data()+4,flags,4);put(h.data()+8,space);put(h.data()+16,std::bit_cast<std::uint64_t>(c->key));put(h.data()+24,previous);put(h.data()+32,payload.size());put(h.data()+40,crc(payload),4);put(h.data()+44,crc({reinterpret_cast<const char*>(h.data()),44}),4);
      encoded.append(reinterpret_cast<const char*>(h.data()),h.size());encoded.append(payload);updates.push_back({{space,c->key},c->value?at:0,at});previous=at;
    }
    // Allocate all directory/undo storage before touching disk or live offsets.
    undo_.reserve(undo_.size()+updates.size());for(const auto& u:updates)if(index_.try_emplace(u.key,0).second)++empty_entries_;
    write(end_,encoded);end_+=encoded.size();
    for(std::size_t i=0;i<updates.size();++i){auto& current=index_.find(updates[i].key)->second;undo_.push_back({updates[i].key,current,head_});assign(current,updates[i].value);head_=updates[i].head;}
  }
  void restore(std::uint64_t target){
    if(target==head_)return;
    const bool ancestor=std::any_of(undo_.begin(),undo_.end(),[&](auto&u){return u.head==target;});
    if(!ancestor){auto rebuilt=reconstruct(target,end_);index_.swap(rebuilt);empty_entries_=0;head_=target;undo_.clear();return;}
    while(head_!=target){auto u=undo_.back();undo_.pop_back();assign(index_.find(u.key)->second,u.value);head_=u.head;}
  }
  void rollback(){restore(durable_);undo_.clear();purge_empty();}
  void commit(){
    if(generation_==UINT64_MAX)throw std::overflow_error("generation exhausted");const auto next=generation_+1;
    if(framed_){
      if(!pending_valid_){pending_={};pending_block_=(end_-8192)/frame_payload;pending_used_=0;pending_valid_=true;}
      if(policy_==CommitSyncPolicy::Ordered){flush_frame(false,0);sync();flush_frame(true,next,true);}
      else flush_frame(true,next);
      sync();generation_=next;durable_=head_;end_=8192+(pending_block_+1)*frame_payload;pending_valid_=false;pending_used_=0;undo_.clear();purge_empty();return;
    }
    if(policy_==CommitSyncPolicy::Ordered)sync();auto p=root(next);write((next%2)*4096,{reinterpret_cast<const char*>(p.data()),p.size()});sync();
    generation_=next;durable_=head_;undo_.clear();purge_empty();
  }
  // Copy into an owned cache in physical order. Completed pages can be released
  // after the visitor retains their decoded contents. Keep the first 64 KiB
  // resident for roots/catalog reads during later SQL binding. This never changes data,
  // durability, roots or the normal ordered scan contract.
  void release_before(std::uint64_t at,std::uint64_t& released,bool enabled,bool final=false) const noexcept {
#if defined(POSIX_FADV_DONTNEED)
    at=address(at);
    const auto physical_at=framed_&&at>=8192?physical((at-8192)/frame_payload):at;
    const auto end=physical_at/4096*4096;
    if(enabled && head_==durable_ && end>released && (final||end-released>=262144)) {
      (void)::posix_fadvise(fd_,off_t(released),off_t(end-released),POSIX_FADV_DONTNEED);released=end;
    }
#else
    (void)at;(void)released;(void)enabled;(void)final;
#endif
  }
  void cache_scan(std::uint64_t space,const std::function<bool(std::int64_t,std::string_view)>& visitor) const {
    auto rows=snapshot(space);std::sort(rows.begin(),rows.end(),[](auto a,auto b){return precedes(a.second,b.second);});
    std::uint64_t released=65536,last=65536;bool retain=true;
    for(auto [key,at]:rows){auto value=value_at(space,key,at);retain=visitor(key,value)&&retain;release_before(at,released,retain);last=at;}
    release_before(last,released,retain,true);
  }
  void cache_scan_equal(std::uint64_t left_space,std::uint64_t right_space,
      const std::function<bool(std::int64_t,std::string_view,std::string_view)>& visitor) const {
    struct Pair{std::int64_t key;std::uint64_t left,right;};std::vector<Pair> rows;
    auto l=index_.lower_bound({left_space,INT64_MIN}),r=index_.lower_bound({right_space,INT64_MIN});
    const auto advance=[&](auto& it,std::uint64_t space){while(it!=index_.end()&&it->first.first==space&&!it->second)++it;};
    for(;;){advance(l,left_space);advance(r,right_space);const bool le=l==index_.end()||l->first.first!=left_space,re=r==index_.end()||r->first.first!=right_space;
      if(le||re){if(le!=re)throw Store::KeySetMismatch();break;}
      if(l->first.second!=r->first.second)throw Store::KeySetMismatch();rows.push_back({l->first.second,l->second,r->second});++l;++r;
    }
    std::sort(rows.begin(),rows.end(),[](auto a,auto b){return precedes(a.right,b.right);});
    std::uint64_t released=65536,last=65536;bool retain=true;
    for(auto row:rows){auto a=value_at(left_space,row.key,row.left),b=value_at(right_space,row.key,row.right);retain=visitor(row.key,a,b)&&retain;release_before(row.right,released,retain);last=row.right;}
    release_before(last,released,retain,true);
  }
  std::vector<std::pair<std::int64_t,std::uint64_t>> snapshot(std::uint64_t space)const{
    std::vector<std::pair<std::int64_t,std::uint64_t>> rows;
    for(auto it=index_.lower_bound({space,INT64_MIN});it!=index_.end()&&it->first.first==space;++it)if(it->second)rows.push_back({it->first.second,it->second});return rows;
  }
};
}
