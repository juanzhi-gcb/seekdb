#pragma once
#include <sys/uio.h>
#include <cerrno>
#include <stdexcept>
#include <system_error>
namespace minivectordb::jsonl::detail {
// Mutates only the local iovec descriptors. Payloads stay borrowed and immutable.
// All fragments, including the terminating newline, are offered in one syscall.
template<class Write> void write_vectors(iovec* parts,int count,Write&& write) {
  int first=0;
  while(first<count) {
    while(first<count&&!parts[first].iov_len)++first;
    if(first==count)break;
    const auto n=write(parts+first,count-first);
    if(n<0){if(errno==EINTR)continue;throw std::system_error(errno,std::generic_category(),"JSON response writev");}
    if(!n)throw std::runtime_error("zero-length JSON response writev");
    auto left=static_cast<std::size_t>(n);
    while(first<count && left>=parts[first].iov_len){left-=parts[first].iov_len;++first;}
    if(left){if(first==count)throw std::runtime_error("invalid JSON response writev count");parts[first].iov_base=static_cast<char*>(parts[first].iov_base)+left;parts[first].iov_len-=left;}
  }
}
}
