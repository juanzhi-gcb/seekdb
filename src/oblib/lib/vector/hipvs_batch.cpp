/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hipvs_batch.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#if defined(__linux__)
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char **environ;
#endif

namespace oceanbase { namespace hipvs {
namespace {
void batch_require(bool condition, const char *message)
{
  if (!condition) throw std::invalid_argument(message);
}
uint64_t checked_environment_number(const char *name, uint64_t default_value,
                                    uint64_t minimum, uint64_t maximum, bool required = false)
{
  const char *value = std::getenv(name);
  if (value == nullptr) {
    if (!required) return default_value;
    throw std::invalid_argument(std::string(name) + " is required");
  }
  uint64_t parsed = 0;
  if (*value == '\0') throw std::invalid_argument(std::string(name) + " must be a decimal integer");
  for (const char *digit = value; *digit != '\0'; ++digit) {
    if (*digit < '0' || *digit > '9' || parsed > (maximum - uint64_t(*digit - '0')) / 10) {
      throw std::invalid_argument(std::string(name) + " must be a decimal integer within range");
    }
    parsed = parsed * 10 + uint64_t(*digit - '0');
  }
  if (parsed < minimum || parsed > maximum) {
    throw std::invalid_argument(std::string(name) + " is outside the supported range");
  }
  return parsed;
}

#if defined(__linux__)
struct FileDescriptor {
  int value;
  explicit FileDescriptor(int fd) : value(fd) { if (fd < 0) throw std::runtime_error("hipVS IPC file open failed"); }
  ~FileDescriptor() { close(value); }
  FileDescriptor(const FileDescriptor &) = delete;
};
void write_all(int fd, const void *data, uint64_t size)
{
  const auto *p = static_cast<const char *>(data);
  while (size != 0) {
    const ssize_t n = write(fd, p, std::min<uint64_t>(size, 1ULL << 30));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("hipVS IPC write failed");
    p += n; size -= n;
  }
}
void read_all(int fd, void *data, uint64_t size)
{
  auto *p = static_cast<char *>(data);
  while (size != 0) {
    const ssize_t n = ::read(fd, p, std::min<uint64_t>(size, 1ULL << 30));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("truncated hipVS worker response");
    p += n; size -= n;
  }
}
struct TemporaryExchange {
  std::string directory, request, response;
  TemporaryExchange()
  {
    const char *tmp = std::getenv("TMPDIR");
    std::string pattern = std::string(tmp == nullptr ? "/tmp" : tmp) + "/seekdb-hipvs-worker-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
    if (mkdtemp(name.data()) == nullptr) throw std::runtime_error("cannot create private hipVS worker directory");
    try {
      directory = name.data();
      request = directory + "/request"; response = directory + "/response";
    } catch (...) { rmdir(name.data()); throw; }
  }
  ~TemporaryExchange() { unlink(request.c_str()); unlink(response.c_str()); rmdir(directory.c_str()); }
};
struct OwnedChild {
  pid_t pid = -1;
  ~OwnedChild()
  {
    if (pid > 0) {
      kill(-pid, SIGKILL);
      while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
  }
  void wait(uint32_t seconds)
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    for (;;) {
      // Retain the zombie leader until RAII cleanup. Its PID cannot be reused
      // while response validation may still need to kill the owned group.
      siginfo_t status{};
      const int result = waitid(P_PID, pid, &status, WEXITED | WNOHANG | WNOWAIT);
      if (result == 0 && status.si_pid == pid) {
        if (status.si_code != CLD_EXITED || status.si_status != 0)
          throw std::runtime_error("hipVS worker exited unsuccessfully or was killed by a signal; no CPU fallback");
        return;
      }
      if (result < 0 && errno != EINTR) {
        if (errno == ECHILD) pid = -1;
        throw std::runtime_error("cannot reap hipVS worker");
      }
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("hipVS worker timeout; no CPU fallback");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
};
#endif

}

BuildBackend selected_build_backend()
{
  const char *backend = std::getenv("SEEKDB_HNSW_BUILD_BACKEND");
  if (backend == nullptr || std::strcmp(backend, "cpu") == 0) return BuildBackend::CPU;
  if (std::strcmp(backend, "hipvs") == 0) return BuildBackend::HIPVS;
  throw std::invalid_argument("SEEKDB_HNSW_BUILD_BACKEND must be cpu or hipvs");
}

BuildOptions BuildOptions::from_environment()
{
  BuildOptions options{};
  const char *path = std::getenv("SEEKDB_HIPVS_BRIDGE");
  batch_require(path != nullptr && path[0] == '/', "SEEKDB_HIPVS_BRIDGE must be an absolute library path");
  options.library_path = path;
  const char *worker = std::getenv("SEEKDB_HIPVS_WORKER");
  batch_require(worker == nullptr || worker[0] == '/', "SEEKDB_HIPVS_WORKER must be an absolute executable path");
  options.worker_path = worker == nullptr ? "" : worker;
  options.worker_timeout_seconds = static_cast<uint32_t>(checked_environment_number(
      "SEEKDB_HIPVS_WORKER_TIMEOUT_SECONDS", 1800, 1, std::numeric_limits<int>::max()));
  options.input_limit = checked_environment_number("SEEKDB_HIPVS_MAX_INPUT_BYTES", 1ULL << 30, 1,
      std::numeric_limits<uint64_t>::max());
  options.export_limit = checked_environment_number("SEEKDB_HIPVS_MAX_EXPORT_BYTES", 1ULL << 30, 1,
      std::numeric_limits<uint64_t>::max());
  options.cpu_threads = static_cast<int>(checked_environment_number("SEEKDB_HIPVS_CPU_THREADS", 0, 1,
      std::numeric_limits<int>::max(), true));
  options.device = static_cast<int>(checked_environment_number("SEEKDB_HIPVS_DEVICE", 0, 0,
      std::numeric_limits<int>::max()));
  return options;
}

GpuLibrary::GpuLibrary(const std::string &path, const std::string &worker_path, uint32_t timeout_seconds)
    : library_path_(path), worker_path_(worker_path), timeout_seconds_(timeout_seconds)
{
  batch_require(!path.empty() && path.front() == '/', "SEEKDB_HIPVS_BRIDGE must be an absolute library path");
  if (worker_path_.empty()) worker_path_ = path.substr(0, path.find_last_of('/') + 1) + "seekdb_hipvs_worker";
  batch_require(worker_path_.front() == '/' && timeout_seconds_ > 0 && timeout_seconds_ <= uint32_t(std::numeric_limits<int>::max()),
                "hipVS worker path/timeout is invalid");
#if !defined(__linux__)
  throw std::runtime_error("hipVS backend requires an AMD ROCm Linux environment");
#else
  struct stat info{};
  if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
    throw std::runtime_error("cannot load requested hipVS bridge: not a regular library file");
#endif
}

GpuLibrary::~GpuLibrary() = default;

std::vector<uint8_t> GpuLibrary::build(const SeekdbHipvsBuildConfig &config,
    const float *vectors, uint64_t export_limit, SeekdbHipvsBuildStats &stats) const
{
  stats = {};
#if !defined(__linux__)
  throw std::runtime_error("hipVS backend requires an AMD ROCm Linux environment");
#else
  batch_require(export_limit > 0 && export_limit <= uint64_t(std::numeric_limits<off_t>::max()) - sizeof(SeekdbHipvsWorkerResponse),
                "invalid hipVS export size limit");
  batch_require(config.size == sizeof(config) && config.dimension > 0 && config.rows > 0 && vectors != nullptr &&
                config.rows <= std::numeric_limits<size_t>::max() / sizeof(float) / config.dimension,
                "invalid hipVS worker input size");
  SeekdbHipvsWorkerRequest request{};
  request.magic = SEEKDB_HIPVS_WORKER_REQUEST_MAGIC;
  request.version = SEEKDB_HIPVS_WORKER_PROTOCOL_VERSION;
  request.header_size = sizeof(request);
  request.config = config;
  request.input_bytes = config.rows * config.dimension * sizeof(float);
  request.max_export_bytes = export_limit;
  batch_require(request.input_bytes <= uint64_t(std::numeric_limits<off_t>::max()) - sizeof(request),
                "hipVS worker request size overflow");
  TemporaryExchange exchange;
  FileDescriptor input(open(exchange.request.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  FileDescriptor response(open(exchange.response.c_str(), O_RDONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (fchmod(input.value, 0600) != 0 || fchmod(response.value, 0600) != 0)
    throw std::runtime_error("cannot secure hipVS IPC permissions");
  write_all(input.value, &request, sizeof(request));
  write_all(input.value, vectors, request.input_bytes);
  char *arguments[] = {const_cast<char *>(worker_path_.c_str()), const_cast<char *>(library_path_.c_str()),
      const_cast<char *>(exchange.request.c_str()), const_cast<char *>(exchange.response.c_str()), nullptr};
  struct SpawnAttributes {
    posix_spawnattr_t value;
    SpawnAttributes() {
      if (posix_spawnattr_init(&value) != 0) throw std::runtime_error("cannot initialize hipVS spawn attributes");
      if (posix_spawnattr_setpgroup(&value, 0) != 0 || posix_spawnattr_setflags(&value, POSIX_SPAWN_SETPGROUP) != 0) {
        posix_spawnattr_destroy(&value); throw std::runtime_error("cannot isolate hipVS worker process group");
      }
    }
    ~SpawnAttributes() { posix_spawnattr_destroy(&value); }
  } attributes;
  OwnedChild child;
  const int spawned = posix_spawn(&child.pid, worker_path_.c_str(), nullptr, &attributes.value, arguments, environ);
  if (spawned != 0) { child.pid = -1; throw std::runtime_error(std::string("cannot spawn hipVS worker: ") + std::strerror(spawned)); }
  child.wait(timeout_seconds_);
  struct stat info{};
  if (fstat(response.value, &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_size < static_cast<off_t>(sizeof(SeekdbHipvsWorkerResponse)) ||
      uint64_t(info.st_size) - sizeof(SeekdbHipvsWorkerResponse) > export_limit)
    throw std::runtime_error("invalid hipVS worker response file length");
  SeekdbHipvsWorkerResponse reply{};
  read_all(response.value, &reply, sizeof(reply));
  batch_require(reply.magic == SEEKDB_HIPVS_WORKER_RESPONSE_MAGIC &&
      reply.version == SEEKDB_HIPVS_WORKER_PROTOCOL_VERSION && reply.header_size == sizeof(reply) && reply.reserved == 0 &&
      reply.output_bytes <= export_limit && reply.output_bytes == uint64_t(info.st_size) - sizeof(reply) &&
      std::memchr(reply.error, '\0', sizeof(reply.error)) != nullptr,
      "incompatible or malformed hipVS worker response");
  if (reply.status != 0) throw std::runtime_error(std::string("hipVS build failed without CPU fallback: ") + reply.error);
  const auto &reported = reply.stats;
  batch_require(reported.size == sizeof(reported) && reported.backend == SEEKDB_HIPVS_AMD_BACKEND &&
      reported.gpu_build_submissions == 1 && reported.synchronized == 1 &&
      std::isfinite(reported.gpu_build_ms) && reported.gpu_build_ms > 0 &&
      std::isfinite(reported.bridge_wall_ms) && reported.bridge_wall_ms > 0 &&
      reported.device_name[0] != '\0' && std::memchr(reported.device_name, '\0', sizeof(reported.device_name)) != nullptr &&
      reported.version[0] != '\0' && std::memchr(reported.version, '\0', sizeof(reported.version)) != nullptr &&
      reply.output_bytes > 0 && reply.output_bytes <= std::numeric_limits<size_t>::max(),
      "hipVS worker did not report a completed AMD GPU build");
  std::vector<uint8_t> bytes(static_cast<size_t>(reply.output_bytes));
  read_all(response.value, bytes.data(), bytes.size());
  stats = reported;
  return bytes;
#endif
}

BuildBatch::BuildBatch(int dimension, uint64_t input_limit)
    : dimension_(dimension), input_limit_(input_limit)
{
  batch_require(dimension > 0 && input_limit > 0, "invalid hipVS batch configuration");
}

void BuildBatch::append(const float *vectors, const int64_t *ids, int dimension, int64_t count)
{
  batch_require(dimension == dimension_ && count >= 0, "hipVS batch dimension/count mismatch");
  if (count == 0) return;
  batch_require(vectors != nullptr && ids != nullptr, "null hipVS batch input");
  const uint64_t row_bytes = uint64_t(dimension_) * sizeof(float) + sizeof(int64_t);
  batch_require(uint64_t(count) <= std::numeric_limits<uint32_t>::max() - ids_.size(), "too many GPU graph nodes");
  const uint64_t new_count = ids_.size() + uint64_t(count);
  batch_require(new_count <= input_limit_ / row_bytes, "hipVS batch exceeds SEEKDB_HIPVS_MAX_INPUT_BYTES");
  for (int64_t i = 0; i < count; ++i) batch_require(ids[i] >= 0, "seekdb VID must be nonnegative");
  const uint64_t values = uint64_t(count) * dimension_;
  for (uint64_t i = 0; i < values; ++i) batch_require(std::isfinite(vectors[i]), "non-finite hipVS input vector");
  // Reserve both arrays before changing either logical size. Geometric capacity
  // avoids quadratic copying when seekdb's DDL pipeline appends one row at a time.
  if (new_count > ids_.capacity()) {
    const uint64_t capacity = std::min(input_limit_ / row_bytes,
        std::max(new_count, std::max<uint64_t>(1024, ids_.capacity() * 2)));
    vectors_.reserve(capacity * dimension_);
    ids_.reserve(capacity);
  }
  vectors_.insert(vectors_.end(), vectors, vectors + values);
  ids_.insert(ids_.end(), ids, ids + count);
}

void BuildBatch::validate_ids() const
{
  std::unordered_set<int64_t> unique;
  unique.reserve(ids_.size());
  for (const auto id : ids_) batch_require(unique.insert(id).second, "duplicate seekdb VID in GPU batch");
}

std::shared_ptr<vsag::Index> BuildBatch::finalize(const std::string &library_path,
    const ImportConfig &config, int ef_construction, int threads, int device,
    uint64_t export_limit, SeekdbHipvsBuildStats &stats, vsag::Allocator *allocator,
    const std::string &worker_path, uint32_t timeout_seconds) const
{
  stats = {};
  batch_require(!ids_.empty() && config.dim == dimension_ && ef_construction > 0 && threads > 0 && device >= 0,
          "invalid hipVS finalization configuration");
  validate_ids();
  SeekdbHipvsBuildConfig request{};
  request.size = sizeof(request);
  request.rows = ids_.size();
  request.dimension = dimension_;
  request.max_degree = config.max_degree;
  request.ef_construction = ef_construction;
  request.metric = config.metric == "l2" ? SEEKDB_HIPVS_L2 : SEEKDB_HIPVS_IP;
  request.cpu_threads = threads;
  request.device = device;
  GpuLibrary library(library_path, worker_path, timeout_seconds);
  SeekdbHipvsBuildStats completed{};
  const auto bytes = library.build(request, vectors_.data(), export_limit, completed);
  auto index = import_hnsw(bytes, ids_, config, allocator);
  stats = completed;
  return index;
}

} }
