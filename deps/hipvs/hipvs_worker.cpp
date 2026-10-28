/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// Deliberately standalone: only the C bridge ABI and system/standard libraries.
#include "hipvs_bridge_abi.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
struct File {
  int fd;
  File(const char *path, int flags) : fd(open(path, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK))
  {
    struct stat info{};
    if (fd < 0) throw std::runtime_error("cannot open hipVS worker IPC file");
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || (info.st_mode & 0777) != 0600 || info.st_uid != geteuid()) {
      close(fd); throw std::runtime_error("unsafe hipVS worker IPC file");
    }
  }
  ~File() { close(fd); }
};
void transfer(int fd, void *data, uint64_t length, bool writing)
{
  auto *p = static_cast<char *>(data);
  while (length != 0) {
    const auto chunk = std::min<uint64_t>(length, 1ULL << 30);
    const ssize_t n = writing ? write(fd, p, chunk) : read(fd, p, chunk);
    if (n < 0 && errno == EINTR) continue;
    require(n > 0, "truncated or unwritable hipVS worker IPC file");
    p += n; length -= n;
  }
}
struct Library {
  void *handle = nullptr;
  ~Library() { if (handle != nullptr) dlclose(handle); }
};
struct Output {
  int fd;
  uint64_t limit, length = 0;
  bool failed = false;
};
int output(void *context, const void *bytes, uint64_t length)
{
  auto &out = *static_cast<Output *>(context);
  try {
    require(!out.failed && (bytes != nullptr || length == 0) && length <= out.limit - out.length,
            "hipVS worker export exceeds output limit");
    transfer(out.fd, const_cast<void *>(bytes), length, true);
    out.length += length;
    return 0;
  } catch (...) { out.failed = true; return 1; }
}
}

int main(int argc, char **argv)
{
  // No IPC descriptor is inherited. Close seekdb sockets/logs before opening
  // request files or loading any GPU runtime; avoid newer glibc closefrom APIs.
  DIR *descriptors = opendir("/proc/self/fd");
  if (descriptors == nullptr) return 2;
  const int directory_fd = dirfd(descriptors);
  while (const dirent *entry = readdir(descriptors)) {
    char *end = nullptr;
    const long fd = std::strtol(entry->d_name, &end, 10);
    if (*entry->d_name != '\0' && *end == '\0' && fd > 2 && fd != directory_fd && fd <= std::numeric_limits<int>::max())
      close(static_cast<int>(fd));
  }
  closedir(descriptors);
  // Named paths survive profiler wrappers that close inherited descriptors.
  if (argc != 4 || argv[1][0] != '/' || argv[2][0] != '/' || argv[3][0] != '/') return 2;
  try {
    File response(argv[3], O_WRONLY);
    require(ftruncate(response.fd, 0) == 0, "cannot truncate hipVS response");
    SeekdbHipvsWorkerResponse reply{};
    reply.magic = SEEKDB_HIPVS_WORKER_RESPONSE_MAGIC;
    reply.version = SEEKDB_HIPVS_WORKER_PROTOCOL_VERSION;
    reply.header_size = sizeof(reply);
    reply.status = 1;
    try {
      File input(argv[2], O_RDONLY);
      struct stat info{};
      require(fstat(input.fd, &info) == 0 && info.st_size >= static_cast<off_t>(sizeof(SeekdbHipvsWorkerRequest)),
              "truncated hipVS worker request header");
      SeekdbHipvsWorkerRequest request{};
      transfer(input.fd, &request, sizeof(request), false);
      require(request.magic == SEEKDB_HIPVS_WORKER_REQUEST_MAGIC &&
              request.version == SEEKDB_HIPVS_WORKER_PROTOCOL_VERSION && request.header_size == sizeof(request),
              "incompatible hipVS worker request protocol");
      const auto &config = request.config;
      require(config.size == sizeof(config) && config.reserved == 0 && config.dimension > 0 && config.rows > 0 &&
              config.rows <= std::numeric_limits<uint32_t>::max() && config.max_degree >= 8 && config.max_degree <= 64 &&
              config.ef_construction >= config.max_degree && config.ef_construction <= uint32_t(std::numeric_limits<int>::max()) &&
              config.cpu_threads > 0 && config.cpu_threads <= uint32_t(std::numeric_limits<int>::max()) && config.device >= 0 &&
              (config.metric == SEEKDB_HIPVS_L2 || config.metric == SEEKDB_HIPVS_IP), "invalid hipVS worker configuration");
      require(config.rows <= std::numeric_limits<size_t>::max() / sizeof(float) / config.dimension,
              "hipVS worker input size overflow");
      const uint64_t size = config.rows * config.dimension * sizeof(float);
      require(request.input_bytes == size && size <= uint64_t(std::numeric_limits<off_t>::max()) - sizeof(request) &&
              uint64_t(info.st_size) == sizeof(request) + size, "hipVS worker request length mismatch");
      require(request.max_export_bytes > 0 && request.max_export_bytes <= uint64_t(std::numeric_limits<off_t>::max()) - sizeof(reply),
              "invalid hipVS worker export limit");
      std::vector<float> vectors(size / sizeof(float));
      transfer(input.fd, vectors.data(), size, false);
      for (const float value : vectors) require(std::isfinite(value), "non-finite hipVS worker input");
      Library library;
      library.handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
      if (library.handle == nullptr) throw std::runtime_error(std::string("cannot load requested hipVS bridge: ") + dlerror());
      const auto getter = reinterpret_cast<SeekdbHipvsGetApi>(dlsym(library.handle, "seekdb_hipvs_get_api_v1"));
      const auto *api = getter == nullptr ? nullptr : getter();
      require(api != nullptr && api->version == SEEKDB_HIPVS_ABI_VERSION && api->size == sizeof(*api) && api->build != nullptr,
              "requested hipVS bridge has an incompatible ABI");
      transfer(response.fd, &reply, sizeof(reply), true);
      Output out{response.fd, request.max_export_bytes};
      reply.stats.size = sizeof(reply.stats);
      const int status = api->build(&config, vectors.data(), output, &out, &reply.stats, reply.error, sizeof(reply.error));
      reply.error[sizeof(reply.error) - 1] = '\0';
      if (status != 0 || out.failed) {
        if (out.failed) std::snprintf(reply.error, sizeof(reply.error), "hipVS worker export rejected (size limit or I/O)");
        throw std::runtime_error(reply.error[0] == '\0' ? "hipVS bridge build failed" : reply.error);
      }
      reply.output_bytes = out.length;
      reply.status = 0;
    } catch (const std::exception &error) {
      reply.stats = {}; reply.output_bytes = 0;
      std::snprintf(reply.error, sizeof(reply.error), "%s", error.what());
    } catch (...) {
      reply.stats = {}; reply.output_bytes = 0;
      std::snprintf(reply.error, sizeof(reply.error), "unknown hipVS worker build failure");
    }
    require(lseek(response.fd, 0, SEEK_SET) == 0, "cannot rewind hipVS response");
    transfer(response.fd, &reply, sizeof(reply), true);
    require(ftruncate(response.fd, sizeof(reply) + reply.output_bytes) == 0, "cannot finish hipVS response");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "hipVS worker: %s\n", error.what());
    return 2;
  }
}
