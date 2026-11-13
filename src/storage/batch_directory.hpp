#pragma once
#include "packed_codec.hpp"
#include "page_checksum.hpp"
#include "minivectordb/store.hpp"
#include <bit>
#include <limits>
#include <span>
namespace minivectordb::detail::batchdir {
inline constexpr std::size_t header_size=48,capacity=128;
inline void put(char* p,std::uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)p[i]=char(v>>(i*8));}
inline std::uint64_t get(const char* p,unsigned n){std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(static_cast<unsigned char>(p[i]))<<(i*8);return v;}
inline std::uint32_t crc(std::string_view s){return ~page_crc32_span(0xffffffffU,reinterpret_cast<const unsigned char*>(s.data()),s.size());}
struct Entry {std::int64_t key=0;std::uint32_t offset=0,checksum=0,length=0;bool compressed=false,deleted=false;};
struct Header {std::uint64_t space=0,previous=0;std::int64_t base=0;std::uint32_t count=0,bytes=0,prefix=0;unsigned width=0;};
struct Directory:Header {std::array<Entry,capacity> entries{};};
// Independent row payloads: only repeated record metadata is shared. End
// offsets make arbitrary slot lookup O(1), without decompressing other rows.
inline bool encode(std::uint64_t space,std::uint64_t previous,std::span<const Store::Mutation* const> rows,std::string& out){
 out.clear();if(rows.size()<2||rows.size()>capacity)return false;
 const auto base=rows.front()->key;std::uint64_t last=0;std::size_t raw_bytes=0;
 for(std::size_t i=0;i<rows.size();++i){const auto* row=rows[i];const auto delta=std::uint64_t(row->key)-std::uint64_t(base);
  if(row->key<base || delta>32767 || (i&&delta<=last))return false;
  if(row->value){if(row->value->size()>Store::max_batch_bytes-raw_bytes)return false;raw_bytes+=row->value->size();}last=delta;
 }
 struct Pending {std::uint16_t key=0;std::uint32_t end=0,checksum=0;};std::array<Pending,capacity> entries;
 std::string packed,payloads;
 for(std::size_t i=0;i<rows.size();++i){const auto* row=rows[i];auto& e=entries[i];e.key=static_cast<std::uint16_t>(std::uint64_t(row->key)-std::uint64_t(base));
  if(row->value){std::string_view payload=*row->value;if(pack_bytes(payload,packed,true,true)){payload=packed;e.key|=0x8000;}e.checksum=crc(payload);payloads.append(payload);}else e.checksum=UINT32_MAX;
  e.end=static_cast<std::uint32_t>(payloads.size());
 }
 const unsigned width=payloads.size()<=65535?2:4;const auto entry_size=6+width;out.assign(header_size+rows.size()*entry_size,'\0');
 for(std::size_t i=0;i<rows.size();++i){auto* e=out.data()+header_size+i*entry_size;put(e,entries[i].key,2);put(e+2,entries[i].end,width);put(e+2+width,entries[i].checksum,4);}
 auto* h=out.data();std::memcpy(h,"JBA2",4);put(h+4,rows.size(),4);put(h+8,space,8);put(h+16,std::bit_cast<std::uint64_t>(base),8);put(h+24,previous,8);put(h+32,payloads.size(),4);put(h+36,crc(std::string_view(out).substr(header_size)),4);put(h+40,width,4);put(h+44,crc(std::string_view(out).substr(0,44)),4);out+=payloads;return true;
}
// Internal point-read path: the storage layer MUST validate the containing
// frame checksums before calling this and entry(). It avoids re-checksumming
// unrelated descriptors on every random access. Recovery uses header()/parse().
inline Header header_from_checked_pages(std::string_view prefix,std::uint64_t available){
 auto bad=[](){throw std::runtime_error("invalid journal batch directory");};
 if(prefix.size()<header_size || std::memcmp(prefix.data(),"JBA2",4) || get(prefix.data()+44,4)!=crc(prefix.substr(0,44)))bad();
 Header result;result.count=static_cast<std::uint32_t>(get(prefix.data()+4,4));result.width=static_cast<unsigned>(get(prefix.data()+40,4));if(result.count<2||result.count>capacity||(result.width!=2&&result.width!=4))bad();
 result.prefix=static_cast<std::uint32_t>(header_size+result.count*(6+result.width));const auto body=get(prefix.data()+32,4);
 if(body>Store::max_batch_bytes || (result.width==2&&body>65535) || result.prefix+body>available)bad();
 result.space=get(prefix.data()+8,8);result.previous=get(prefix.data()+24,8);result.base=std::bit_cast<std::int64_t>(get(prefix.data()+16,8));result.bytes=static_cast<std::uint32_t>(result.prefix+body);return result;
}
inline Header header(std::string_view prefix,std::uint64_t available){
 auto result=header_from_checked_pages(prefix,available);
 if(prefix.size()<result.prefix || get(prefix.data()+36,4)!=crc(prefix.substr(header_size,result.prefix-header_size)))throw std::runtime_error("invalid journal batch directory checksum");return result;
}
inline Entry entry(const Header& h,std::string_view prefix,unsigned slot){
 auto bad=[](){throw std::runtime_error("invalid journal batch entry");};if(slot>=h.count||prefix.size()<h.prefix)bad();const auto step=6+h.width;const auto* e=prefix.data()+header_size+slot*step;
 const auto key=get(e,2),delta=key&32767,end=get(e+2,h.width),begin=slot?get(e-step+2,h.width):0;
 if((slot==0&&delta) || (slot&&delta<=(get(e-step,2)&32767)) || delta>std::uint64_t(INT64_MAX)-std::uint64_t(h.base) || end<begin || end>h.bytes-h.prefix || (slot+1==h.count&&end!=h.bytes-h.prefix))bad();
 Entry result;result.key=std::bit_cast<std::int64_t>(std::uint64_t(h.base)+delta);result.offset=h.prefix+static_cast<std::uint32_t>(begin);result.length=static_cast<std::uint32_t>(end-begin);result.checksum=static_cast<std::uint32_t>(get(e+2+h.width,4));result.compressed=(key&0x8000)!=0;result.deleted=!result.length&&result.checksum==UINT32_MAX;
 if((result.compressed&&(result.length<6||result.length>4064)) || (!result.length&&result.checksum!=0&&result.checksum!=UINT32_MAX))bad();return result;
}
inline Directory parse(std::string_view prefix,std::uint64_t available){
 Directory result;static_cast<Header&>(result)=header(prefix,available);for(unsigned i=0;i<result.count;++i)result.entries[i]=entry(result,prefix,i);return result;
}
inline std::optional<std::string> value(const Entry& e,std::string_view image){
 if(e.offset>image.size() || e.length>image.size()-e.offset)throw std::runtime_error("invalid journal batch slot");if(e.deleted)return {};
 const auto payload=image.substr(e.offset,e.length);if(crc(payload)!=e.checksum)throw std::runtime_error("invalid journal batch payload");return e.compressed?unpack_bytes(payload,4064,true):std::string(payload);
}
inline std::optional<std::string> value(const Directory& d,std::string_view image,std::size_t slot){if(slot>=d.count)throw std::runtime_error("invalid journal batch slot");return value(d.entries[slot],image);}
}
