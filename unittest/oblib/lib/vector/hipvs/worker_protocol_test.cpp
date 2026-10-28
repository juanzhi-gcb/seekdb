/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// TESTONLY synthetic transport/lifecycle tests. No GPU evidence is produced.
#include "hipvs_batch.h"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace oceanbase::hipvs;
static void check(bool ok) { if (!ok) throw std::runtime_error("TESTONLY worker protocol contract failed"); }
template <typename F> static void rejects(F fn) { bool failed=false; try {fn();} catch(const std::exception &) {failed=true;} check(failed); }
static int synthetic(int argc, char **argv)
{
  check(argc == 4);
  const char *mode = std::getenv("HIPVS_TESTONLY_MODE");
  std::string request_path(argv[2]);
  const auto directory=request_path.substr(0,request_path.find_last_of('/'));
  struct stat info{};
  check(stat(directory.c_str(),&info)==0 && (info.st_mode & 0777)==0700);
  check(lstat(argv[2],&info)==0 && S_ISREG(info.st_mode) && (info.st_mode & 0777)==0600);
  check(lstat(argv[3],&info)==0 && S_ISREG(info.st_mode) && (info.st_mode & 0777)==0600);
  std::ofstream marker(std::string(std::getenv("TMPDIR"))+"/observed");
  pid_t grandchild = -1;
  if(std::strcmp(mode,"wrapper_timeout")==0) {
    grandchild=fork(); check(grandchild>=0);
    if(grandchild==0) { sleep(30); _exit(0); }
  }
  marker << request_path << '\n' << getpid() << '\n' << grandchild << '\n'; marker.close();
  if(std::strcmp(mode,"timeout")==0 || std::strcmp(mode,"wrapper_timeout")==0) { sleep(30); return 0; }
  if(std::strcmp(mode,"signal")==0) { raise(SIGTERM); return 0; }
  if(std::strcmp(mode,"exit")==0) return 7;
  SeekdbHipvsWorkerResponse reply{};
  reply.magic=SEEKDB_HIPVS_WORKER_RESPONSE_MAGIC;
  reply.version=SEEKDB_HIPVS_WORKER_PROTOCOL_VERSION;
  reply.header_size=sizeof(reply);
  if(std::strcmp(mode,"magic")==0) reply.magic=0;
  if(std::strcmp(mode,"version")==0) reply.version++;
  if(std::strcmp(mode,"oversize")==0) reply.output_bytes=UINT64_MAX;
  if(std::strcmp(mode,"error")==0) {reply.status=1; std::strcpy(reply.error,"TESTONLY explicit failure");}
  if(std::strcmp(mode,"unterminated")==0) std::memset(reply.error,'x',sizeof(reply.error));
  std::ofstream response(argv[3],std::ios::binary|std::ios::trunc);
  response.write(reinterpret_cast<char *>(&reply),std::strcmp(mode,"truncated")==0?3:sizeof(reply));
  return 0;
}
int main(int argc,char **argv)
{
  try {
    if(std::getenv("HIPVS_TESTONLY_MODE") != nullptr && argc==4) return synthetic(argc,argv);
    check(argc==3); check(prctl(PR_SET_CHILD_SUBREAPER,1)==0);
    const std::string bridge=argv[1], worker=argv[2];
    char pattern[]="/tmp/seekdb-hipvs-testonly-XXXXXX";
    const char *tmp=mkdtemp(pattern); check(tmp!=nullptr); setenv("TMPDIR",tmp,1);
    SeekdbHipvsBuildConfig config{};
    config.size=sizeof(config); config.rows=1; config.dimension=2; config.max_degree=8;
    config.ef_construction=16; config.metric=SEEKDB_HIPVS_L2; config.cpu_threads=1;
    float vectors[]={1.25f,-2.5f}; SeekdbHipvsBuildStats stats{};
    // The fixture aborts if accidentally loaded into this parent.
    setenv("HIPVS_TESTONLY_PARENT_PID",std::to_string(getpid()).c_str(),1);
    const int source=open("/dev/null",O_RDONLY); check(source>=0);
    check(dup2(source,199)==199); close(source);
    setenv("HIPVS_TESTONLY_INHERITED_FD","199",1);
    GpuLibrary library(bridge,worker,1);
    for(int iteration=0;iteration<3;++iteration) {
      const auto bytes=library.build(config,vectors,128,stats);
      check(std::string(bytes.begin(),bytes.end())=="TESTONLY native bytes");
      check(stats.backend==SEEKDB_HIPVS_AMD_BACKEND); check(fcntl(199,F_GETFD)!=-1);
      check(dlopen(bridge.c_str(),RTLD_NOLOAD|RTLD_NOW)==nullptr);
    }
    // An export rejected by the callback must discard even deliberately stale stats.
    rejects([&]{library.build(config,vectors,2,stats);}); check(stats.backend==0);
    config.device=9; rejects([&]{library.build(config,vectors,128,stats);}); check(stats.backend==0); config.device=0;
    rejects([&]{GpuLibrary missing(bridge,"/no/such/worker",1); missing.build(config,vectors,128,stats);}); check(stats.backend==0);
    for(const auto *mode:{"magic","version","oversize","error","unterminated","truncated","signal","exit","timeout","wrapper_timeout"}) {
      setenv("HIPVS_TESTONLY_MODE",mode,1);
      GpuLibrary broken(bridge,argv[0],1);
      stats.backend=SEEKDB_HIPVS_AMD_BACKEND;
      rejects([&]{broken.build(config,vectors,128,stats);}); check(stats.backend==0);
      std::ifstream marker(std::string(tmp)+"/observed"); std::string path; pid_t pid; marker >> path >> pid; pid_t grandchild=-1; marker >> grandchild;
      check(!path.empty() && access(path.c_str(),F_OK)!=0 && access(path.substr(0,path.find_last_of('/')).c_str(),F_OK)!=0);
      errno=0; check(waitpid(pid,nullptr,WNOHANG)==-1 && errno==ECHILD);
      if(grandchild>0) {
        int status=0; pid_t result=-1;
        for(int i=0;i<100 && result== -1;++i) {
          result=waitpid(grandchild,&status,WNOHANG);
          if(result==0) { result=-1; usleep(10000); }
        }
        const bool terminated=result==grandchild && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL;
        if(!terminated) { kill(grandchild,SIGKILL);waitpid(grandchild,nullptr,0); }
        check(terminated);
      }
      std::cout << "TESTONLY " << mode << " child_pid=" << pid << " reaped=yes cleanup=yes\n";
      unsetenv("HIPVS_TESTONLY_MODE");
      check(library.build(config,vectors,128,stats).size()==21);
    }
    BuildBatch batch(2,1024); int64_t id=17; batch.append(vectors,&id,2,1);
    rejects([&]{batch.finalize(bridge,{2,8,"l2","{}"},16,1,0,128,stats,nullptr,worker,1);}); check(stats.backend==0);
    close(199);
    unlink((std::string(tmp)+"/observed").c_str()); check(rmdir(tmp)==0);
    std::cout << "PASS TESTONLY: isolated real worker, exact FP32 IPC, bounded errors/exports, malformed response, death/timeout/reap/cleanup, repeated recovery and failed-import stats\n";
  } catch(const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n';return 1; }
}
