#define _GNU_SOURCE
#include <dlfcn.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <errno.h>
static void note(const char*kind,size_t bytes,int newline){char b[128];int saved=errno;int n=snprintf(b,sizeof b,"{\"call\":\"%s\",\"bytes\":%zu,\"newline\":%d}\n",kind,bytes,newline);syscall(SYS_write,2,b,n);errno=saved;}
ssize_t write(int fd,const void*p,size_t n){static ssize_t(*real)(int,const void*,size_t);if(!real)real=dlsym(RTLD_NEXT,"write");ssize_t out=real(fd,p,n);if(fd==1)note("write",n,n&&((const char*)p)[n-1]=='\n');return out;}
ssize_t writev(int fd,const struct iovec*p,int n){static ssize_t(*real)(int,const struct iovec*,int);if(!real)real=dlsym(RTLD_NEXT,"writev");ssize_t out=real(fd,p,n);if(fd==1){size_t bytes=0;int newline=0;for(int i=0;i<n;++i){bytes+=p[i].iov_len;if(p[i].iov_len)newline=((const char*)p[i].iov_base)[p[i].iov_len-1]=='\n';}note("writev",bytes,newline);}return out;}
