/* TESTONLY synthetic bridge. No HIP/ROCm and no genuine GPU statistics. */
#include "hipvs_bridge_abi.h"
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
__attribute__((constructor)) static void parent_must_not_load()
{
  const char *parent=std::getenv("HIPVS_TESTONLY_PARENT_PID");
  if(parent!=nullptr && std::atoi(parent)==getpid()) std::abort();
}
static int build(const SeekdbHipvsBuildConfig *config,const float *vectors,SeekdbHipvsWrite write,void *context,
                 SeekdbHipvsBuildStats *stats,char *error,uint64_t capacity)
{
  const char *descriptor = std::getenv("HIPVS_TESTONLY_INHERITED_FD");
  if(descriptor != nullptr && fcntl(std::atoi(descriptor), F_GETFD) != -1) std::abort();
  if(config->device==9 || vectors[0]!=1.25f || vectors[1]!=-2.5f) {
    if(capacity>32) std::strcpy(error,"TESTONLY rejected input/device"); return 1;
  }
  *stats={}; stats->size=sizeof(*stats); stats->backend=SEEKDB_HIPVS_AMD_BACKEND;
  stats->gpu_build_submissions=1;stats->synchronized=1;stats->gpu_build_ms=1;stats->bridge_wall_ms=2;
  std::strcpy(stats->device_name,"TESTONLY SYNTHETIC");std::strcpy(stats->version,"TESTONLY not GPU evidence");
  return write(context,"TESTONLY native bytes",21);
}
extern "C" __attribute__((visibility("default"))) const SeekdbHipvsApi *seekdb_hipvs_get_api_v1()
{
  static const SeekdbHipvsApi api{SEEKDB_HIPVS_ABI_VERSION,sizeof(SeekdbHipvsApi),build};return &api;
}
