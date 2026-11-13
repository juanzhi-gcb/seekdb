#pragma once
#include "journal_store.hpp"
#include "segment_store.hpp"
#include <variant>
namespace minivectordb::detail {
class JournalStore {
  std::variant<std::unique_ptr<LegacyJournalStore>,std::unique_ptr<SegmentStore>> impl_;
  template<class F> decltype(auto) visit(F&& f)const{return std::visit([&](const auto&p)->decltype(auto){return f(*p);},impl_);}
public:
  static bool magic(const unsigned char*p){return LegacyJournalStore::magic(p)||SegmentStore::magic(p);}
  JournalStore(int fd,bool created,std::size_t budget,CommitSyncPolicy policy,bool segments=false){
    if(!created){std::array<unsigned char,4096>a{},b{};auto read=[&](off_t at,auto& data){std::size_t done=0;while(done<data.size()){const auto n=::pread(fd,data.data()+done,data.size()-done,at+off_t(done));if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("journal magic read failed");done+=std::size_t(n);}};read(0,a);read(4096,b);auto valid=[](const auto&p){std::uint32_t stored=0;for(unsigned i=0;i<4;++i)stored|=std::uint32_t(p[32+i])<<(8*i);return SegmentStore::magic(p.data())&&stored==page_checksum(p,32);};segments=valid(a)||valid(b);}
    if(segments)impl_=std::make_unique<SegmentStore>(fd,created,budget,policy);else impl_=std::make_unique<LegacyJournalStore>(fd,created,budget,policy);
  }
  auto head()const{return visit([](auto&s){return s.head();});}
  void begin(){visit([](auto&s){s.begin();});}
  auto value(std::uint64_t space,std::int64_t key)const{return visit([&](auto&s){return s.value(space,key);});}
  auto value_at(std::uint64_t space,std::int64_t key,std::uint64_t at)const{return visit([&](auto&s){return s.value_at(space,key,at);});}
  void apply(std::uint64_t space,std::span<const Store::Mutation> changes){visit([&](auto&s){s.apply(space,changes);});}
  void restore(std::uint64_t at){visit([&](auto&s){s.restore(at);});}
  void rollback(){visit([](auto&s){s.rollback();});}
  void commit(){visit([](auto&s){s.commit();});}
  auto snapshot(std::uint64_t space)const{return visit([&](auto&s){return s.snapshot(space);});}
};
}
