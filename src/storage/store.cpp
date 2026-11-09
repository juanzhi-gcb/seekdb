#include "minivectordb/store.hpp"
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace minivectordb {
namespace {
constexpr std::size_t page_size=4096, payload=32, chunk_size=page_size-payload;
using Page=std::array<unsigned char,page_size>;
constexpr char page_magic[]="MVDBPG01", root_magic[]="MVDBRT01";
enum : std::uint32_t { branch=1, overflow=2 };
std::atomic<std::uint64_t> next_owner{1};
[[noreturn]] void io_error(const char* action) {throw std::system_error(errno,std::generic_category(),action);}
[[noreturn]] void corrupt() {throw std::runtime_error("corrupt MiniVectorDB file");}
void encode(Page& p,std::size_t offset,std::uint64_t value,unsigned bytes=8) {
  for(unsigned i=0;i<bytes;++i) p[offset+i]=static_cast<unsigned char>(value>>(i*8));
}
std::uint64_t decode(const Page& p,std::size_t offset,unsigned bytes=8) {
  std::uint64_t value=0;
  for(unsigned i=0;i<bytes;++i) value|=static_cast<std::uint64_t>(p[offset+i])<<(i*8);
  return value;
}
std::uint32_t checksum(const Page& p,std::size_t checksum_offset) {
  std::uint32_t crc=0xffffffffu;
  for(std::size_t i=0;i<p.size();++i) {
    crc^=(i>=checksum_offset && i<checksum_offset+4)?0:p[i];
    for(int bit=0;bit<8;++bit) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));
  }
  return ~crc;
}
off_t byte_offset(std::uint64_t id) {
  if(id>static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())/page_size) throw std::length_error("database exceeds file offset range");
  return static_cast<off_t>(id*page_size);
}
void write_page(int fd,const Page& p,std::uint64_t id) {
  std::size_t done=0; auto start=byte_offset(id);
  while(done<p.size()) {
    auto n=::pwrite(fd,p.data()+done,p.size()-done,start+static_cast<off_t>(done));
    if(n<0) {if(errno==EINTR) continue; io_error("pwrite");}
    if(n==0) throw std::runtime_error("zero-length pwrite");
    done+=static_cast<std::size_t>(n);
  }
}
Page read_raw(int fd,std::uint64_t id) {
  Page p{}; std::size_t done=0; auto start=byte_offset(id);
  while(done<p.size()) {
    auto n=::pread(fd,p.data()+done,p.size()-done,start+static_cast<off_t>(done));
    if(n<0) {if(errno==EINTR) continue; io_error("pread");}
    if(n==0) corrupt();
    done+=static_cast<std::size_t>(n);
  }
  return p;
}
void sync_fd(int fd) {while(::fsync(fd)!=0) {if(errno!=EINTR) io_error("fsync");}}
Page make_root(std::uint64_t generation,std::uint64_t root,std::uint64_t extent) {
  Page p{}; std::memcpy(p.data(),root_magic,8);
  encode(p,8,generation); encode(p,16,root); encode(p,24,extent);
  encode(p,32,checksum(p,32),4); return p;
}
bool valid_root(const Page& p,std::uint64_t complete_pages) {
  if(std::memcmp(p.data(),root_magic,8)!=0 || decode(p,32,4)!=checksum(p,32)) return false;
  auto root=decode(p,16), extent=decode(p,24);
  return extent>=2 && extent<=complete_pages && (root==0 || (root>=2 && root<extent));
}
std::uint64_t ordered_key(std::int64_t key) {return std::bit_cast<std::uint64_t>(key)^(1ULL<<63);}
std::int64_t signed_key(std::uint64_t key) {return std::bit_cast<std::int64_t>(key^(1ULL<<63));}
unsigned key_index(std::uint64_t key,unsigned depth) {return static_cast<unsigned>((key>>((7-depth)*8))&255);}
}

struct Store::Impl {
  int fd=-1;
  std::uint64_t durable_root=0, root=0, generation=0, next_page=2, epoch=0;
  const std::uint64_t owner=next_owner.fetch_add(1);
  bool active=false, poisoned=false;
  ~Impl() {if(fd>=0) ::close(fd);}
  void usable() const {if(poisoned) throw std::runtime_error("commit outcome uncertain; close and reopen Store");}
  void transaction() const {usable(); if(!active) throw std::logic_error("no active transaction");}
  Page read(std::uint64_t id,std::uint32_t type,unsigned depth=0) const {
    if(id<2 || id>=next_page) corrupt();
    auto p=read_raw(fd,id);
    if(std::memcmp(p.data(),page_magic,8)!=0 || decode(p,24,4)!=checksum(p,24) || decode(p,8,4)!=type) corrupt();
    if(type==branch) {
      if(decode(p,12,4)!=depth) corrupt();
      for(unsigned i=0;i<256;++i) {
        auto child=decode(p,payload+i*8);
        if(child!=0 && (child<2 || child>=id)) corrupt();
      }
    } else {
      auto next=decode(p,16);
      if(decode(p,12,4)>chunk_size || (next!=0 && (next<2 || next>=id))) corrupt();
    }
    return p;
  }
  std::uint64_t append(Page p) {
    // Publish an address only after its complete write. A failed page has no root references.
    auto id=next_page; encode(p,24,checksum(p,24),4); write_page(fd,p,id); ++next_page; return id;
  }
  Page empty_page(std::uint32_t type,unsigned used) const {
    Page p{}; std::memcpy(p.data(),page_magic,8); encode(p,8,type,4); encode(p,12,used,4); return p;
  }
  std::uint64_t write_value(std::string_view value) {
    std::uint64_t next=0; std::size_t end=value.size();
    do {
      const auto start=end>chunk_size?end-chunk_size:0, length=end-start;
      auto p=empty_page(overflow,static_cast<unsigned>(length)); encode(p,16,next);
      if(length) std::memcpy(p.data()+payload,value.data()+start,length);
      next=append(p); end=start;
    } while(end);
    return next;
  }
  std::string value(std::uint64_t id) const {
    std::string result;
    while(id) {auto p=read(id,overflow); result.append(reinterpret_cast<const char*>(p.data()+payload),static_cast<std::size_t>(decode(p,12,4))); id=decode(p,16);}
    return result;
  }
  std::uint64_t find(std::uint64_t key) const {
    auto id=root;
    for(unsigned depth=0;depth<8 && id;++depth) {auto p=read(id,branch,depth); id=decode(p,payload+key_index(key,depth)*8);}
    return id;
  }
  std::uint64_t replace(std::uint64_t id,unsigned depth,std::uint64_t key,std::uint64_t new_value) {
    auto p=id?read(id,branch,depth):empty_page(branch,depth);
    auto offset=payload+key_index(key,depth)*8;
    auto child=depth==7?new_value:replace(decode(p,offset),depth+1,key,new_value);
    encode(p,offset,child);
    bool empty=true;
    for(unsigned i=0;i<256;++i) if(decode(p,payload+i*8)) {empty=false; break;}
    return empty?0:append(p);
  }
  void walk(std::uint64_t id,unsigned depth,std::uint64_t prefix,const std::function<void(std::int64_t,std::string_view)>& visitor) const {
    if(!id) return;
    auto p=read(id,branch,depth);
    for(unsigned i=0;i<256;++i) {
      auto child=decode(p,payload+i*8); if(!child) continue;
      auto key=prefix|(static_cast<std::uint64_t>(i)<<((7-depth)*8));
      if(depth==7) {auto contents=value(child); visitor(signed_key(key),contents);}
      else walk(child,depth+1,key,visitor);
    }
  }
};

Store::Store(const std::filesystem::path& path):impl_(std::make_unique<Impl>()) {
  bool created=false;
  impl_->fd=::open(path.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);
  if(impl_->fd>=0) created=true;
  else {
    if(errno!=EEXIST) io_error("create database");
    impl_->fd=::open(path.c_str(),O_RDWR|O_CLOEXEC);
    if(impl_->fd<0) io_error("open database");
  }
  auto& s=*impl_;
  if(::flock(s.fd,LOCK_EX|LOCK_NB)!=0) io_error("lock database");
  if(created) {
    write_page(s.fd,Page{},1); write_page(s.fd,make_root(0,0,2),0); sync_fd(s.fd);
    auto parent=path.parent_path(); if(parent.empty()) parent=".";
    int directory=::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(directory<0) io_error("open database directory");
    try {sync_fd(directory);} catch(...) {::close(directory); throw;}
    ::close(directory); return;
  }
  struct stat info{}; if(::fstat(s.fd,&info)!=0) io_error("stat database");
  if(info.st_size<static_cast<off_t>(2*page_size)) corrupt();
  auto bytes=static_cast<std::uint64_t>(info.st_size), complete=bytes/page_size;
  auto a=read_raw(s.fd,0), b=read_raw(s.fd,1);
  bool av=valid_root(a,complete), bv=valid_root(b,complete);
  if(!av && !bv) corrupt();
  const auto& selected=(!av || (bv && decode(b,8)>decode(a,8)))?b:a;
  s.generation=decode(selected,8); s.root=s.durable_root=decode(selected,16);
  // Validate the published root against its durable extent. Trailing uncommitted pages are ignored.
  s.next_page=decode(selected,24);
  if(s.root) (void)s.read(s.root,branch,0);
  s.next_page=complete+(bytes%page_size!=0);
}
Store::~Store()=default;
void Store::begin() {
  auto& s=*impl_; s.usable(); if(s.active) throw std::logic_error("transaction already active");
  if(s.epoch==UINT64_MAX) throw std::overflow_error("transaction epoch exhausted");
  ++s.epoch; s.root=s.durable_root; s.active=true;
}
std::optional<std::string> Store::get(std::int64_t key) const {
  auto& s=*impl_; s.usable(); auto id=s.find(ordered_key(key));
  if(!id) return std::nullopt;
  return s.value(id);
}
void Store::put(std::int64_t key,std::string_view value) {
  auto& s=*impl_; s.transaction();
  const auto data=s.write_value(value);
  const auto next=s.replace(s.root,0,ordered_key(key),data);
  s.root=next;
}
bool Store::erase(std::int64_t key) {
  auto& s=*impl_; s.transaction(); auto ordered=ordered_key(key);
  if(!s.find(ordered)) return false;
  const auto next=s.replace(s.root,0,ordered,0); s.root=next; return true;
}
void Store::commit() {
  auto& s=*impl_; s.transaction();
  if(s.generation==UINT64_MAX) throw std::overflow_error("generation exhausted");
  auto next=s.generation+1;
  try {
    sync_fd(s.fd); // All new immutable data reaches stable storage before root publication.
    write_page(s.fd,make_root(next,s.root,s.next_page),next%2);
    sync_fd(s.fd); // A successful return acknowledges the new durable root.
  } catch(...) {s.poisoned=true; throw;}
  s.generation=next; s.durable_root=s.root; s.active=false;
}
void Store::rollback() {auto& s=*impl_; s.transaction(); s.root=s.durable_root; s.active=false;}
Store::Savepoint Store::savepoint() const {
  auto& s=*impl_; s.transaction(); Savepoint point;
  point.root_=s.root; point.epoch_=s.epoch; point.owner_=s.owner; return point;
}
void Store::restore(const Savepoint& point) {
  auto& s=*impl_; s.transaction();
  if(point.owner_!=s.owner || point.epoch_!=s.epoch) throw std::invalid_argument("savepoint belongs to another transaction");
  s.root=point.root_;
}
void Store::scan(const std::function<void(std::int64_t,std::string_view)>& visitor) const {
  auto& s=*impl_; s.usable(); s.walk(s.root,0,0,visitor);
}
}
