#include "diagnostic_stages.hpp"
#include <unistd.h>
namespace {mvdb_diag::Report report;}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){mvdb_diag::Stage s(12);return __real_fsync(fd);}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void*p,size_t n,off_t at){mvdb_diag::Stage s(13);return __real_pread(fd,p,n,at);}
