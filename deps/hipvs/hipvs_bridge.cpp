/*
 * Copyright (c) 2026 OceanBase.
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
#include "hipvs_bridge_abi.h"
#include <hip/hip_runtime.h>
#if !defined(__HIP_PLATFORM_AMD__)
#error "The competition bridge requires the AMD ROCm/HIP platform"
#endif
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/hnsw.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#ifndef SEEKDB_HIPVS_SOURCE_REVISION
#error "Record the verified installed hipVS source revision, or unknown"
#endif
#ifndef SEEKDB_HIPVS_C_LIBRARY_SHA256
#error "Record the actual installed hipVS C library SHA256"
#endif

namespace {
using Clock = std::chrono::steady_clock;
void require(bool condition, const char *message)
{
  if (!condition) throw std::invalid_argument(message);
}
void hip_check(hipError_t status)
{
  if (status != hipSuccess) throw std::runtime_error(hipGetErrorString(status));
}
void cuvs_check(cuvsError_t status, const char *operation)
{
  if (status != CUVS_SUCCESS) {
    const char *detail = cuvsGetLastErrorText();
    throw std::runtime_error(std::string(operation) + ": " +
                             (detail == nullptr ? "hipVS C API failure" : detail));
  }
}
template <typename T, cuvsError_t (*Create)(T *), cuvsError_t (*Destroy)(T)>
struct Handle {
  T value{};
  explicit Handle(const char *operation) { cuvs_check(Create(&value), operation); }
  ~Handle() { Destroy(value); }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
};
struct DeviceGuard {
  int original;
  explicit DeviceGuard(int device)
  {
    hip_check(hipGetDevice(&original));
    hip_check(hipSetDevice(device));
  }
  ~DeviceGuard() { (void)hipSetDevice(original); }
};
struct Event {
  hipEvent_t value{};
  Event() { hip_check(hipEventCreate(&value)); }
  ~Event() { (void)hipEventDestroy(value); }
};
struct TemporaryFile {
  std::string path;
  TemporaryFile()
  {
    const char *directory = std::getenv("TMPDIR");
    std::string pattern = std::string(directory == nullptr ? "/tmp" : directory) + "/seekdb-hipvs-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int descriptor = mkstemp(name.data());
    if (descriptor < 0) throw std::runtime_error("cannot create temporary HNSW export");
    close(descriptor);
    path = name.data();
  }
  ~TemporaryFile() { std::remove(path.c_str()); }
};

int build(const SeekdbHipvsBuildConfig *request, const float *vectors,
          SeekdbHipvsWrite write, void *context, SeekdbHipvsBuildStats *stats,
          char *error, uint64_t error_capacity)
{
  try {
    const auto start = Clock::now();
    require(stats != nullptr && stats->size == sizeof(*stats), "invalid bridge ABI stats");
    // A caller may reuse a previously successful response. Clear it before
    // validating any other input so an early failure cannot retain success.
    *stats = {};
    stats->size = sizeof(*stats);
    require(request != nullptr && request->size == sizeof(*request) && vectors != nullptr &&
            write != nullptr, "invalid bridge ABI request");
    const auto &config = *request;
    require(config.dimension > 0 && config.rows <= std::numeric_limits<uint32_t>::max() &&
            config.max_degree >= 8 && config.max_degree <= 64 &&
            config.ef_construction >= config.max_degree &&
            config.ef_construction <= uint32_t(std::numeric_limits<int>::max()) &&
            config.cpu_threads > 0 &&
            config.cpu_threads <= uint32_t(std::numeric_limits<int>::max()) &&
            (config.metric == SEEKDB_HIPVS_L2 || config.metric == SEEKDB_HIPVS_IP),
            "unsupported GPU HNSW configuration");
    const uint64_t intermediate_degree = std::max<uint64_t>(128, 4 * config.max_degree);
    require(config.rows > intermediate_degree, "GPU NN-descent build requires N > max(128, 4*M); no CPU fallback");
    require(config.rows <= std::numeric_limits<size_t>::max() / config.dimension / sizeof(float),
            "GPU vector matrix size overflow");
    for (uint64_t i = 0; i < config.rows * config.dimension; ++i) {
      require(std::isfinite(vectors[i]), "non-finite GPU input vector");
    }
    DeviceGuard device(config.device);
    hipDeviceProp_t properties{};
    hip_check(hipGetDeviceProperties(&properties, config.device));
    std::snprintf(stats->device_name, sizeof(stats->device_name), "%s", properties.name);
    std::snprintf(stats->version, sizeof(stats->version), "c_api_sha256=%s source=%s",
                  SEEKDB_HIPVS_C_LIBRARY_SHA256, SEEKDB_HIPVS_SOURCE_REVISION);
    Handle<cuvsResources_t, cuvsResourcesCreate, cuvsResourcesDestroy> resources("cuvsResourcesCreate");
    hipStream_t stream{};
    cuvs_check(cuvsStreamGet(resources.value, &stream), "cuvsStreamGet");
    int64_t shape[2] = {int64_t(config.rows), int64_t(config.dimension)};
    DLManagedTensor dataset{};
    dataset.dl_tensor.data = const_cast<float *>(vectors);
    dataset.dl_tensor.device = {kDLCPU, 0};
    dataset.dl_tensor.ndim = 2;
    dataset.dl_tensor.dtype = {kDLFloat, 32, 1};
    dataset.dl_tensor.shape = shape;
    // The C API only borrows this contiguous host FP32 tensor during the call.
    Handle<cuvsCagraIndexParams_t, cuvsCagraIndexParamsCreate, cuvsCagraIndexParamsDestroy>
        graph_params("cuvsCagraIndexParamsCreate");
    graph_params.value->metric = config.metric == SEEKDB_HIPVS_L2 ? L2Expanded : InnerProduct;
    graph_params.value->graph_degree = 2 * config.max_degree;
    graph_params.value->intermediate_graph_degree = intermediate_degree;
    graph_params.value->build_algo = NN_DESCENT;
    graph_params.value->nn_descent_niter = 20;
    graph_params.value->compression = nullptr;
    Handle<cuvsCagraIndex_t, cuvsCagraIndexCreate, cuvsCagraIndexDestroy> graph("cuvsCagraIndexCreate");
    Event begin, end;
    hip_check(hipEventRecord(begin.value, stream));
    // The host-matrix overload transfers FP32 vectors and builds the core graph on GPU.
    cuvs_check(cuvsCagraBuild(resources.value, graph_params.value, &dataset, graph.value), "cuvsCagraBuild");
    stats->gpu_build_submissions = 1;
    hip_check(hipEventRecord(end.value, stream));
    hip_check(hipEventSynchronize(end.value));
    float gpu_ms = 0;
    hip_check(hipEventElapsedTime(&gpu_ms, begin.value, end.value));
    stats->gpu_build_ms = gpu_ms;
    int64_t graph_rows = 0, graph_degree = 0, graph_dims = 0;
    cuvs_check(cuvsCagraIndexGetSize(graph.value, &graph_rows), "cuvsCagraIndexGetSize");
    cuvs_check(cuvsCagraIndexGetGraphDegree(graph.value, &graph_degree), "cuvsCagraIndexGetGraphDegree");
    cuvs_check(cuvsCagraIndexGetDims(graph.value, &graph_dims), "cuvsCagraIndexGetDims");
    require(graph_rows == int64_t(config.rows) && graph_degree == 2 * config.max_degree &&
            graph_dims == config.dimension,
            "hipVS GPU graph count/degree mismatch");
    Handle<cuvsHnswIndexParams_t, cuvsHnswIndexParamsCreate, cuvsHnswIndexParamsDestroy>
        hnsw_params("cuvsHnswIndexParamsCreate");
    hnsw_params.value->hierarchy = CPU;
    hnsw_params.value->ef_construction = int(config.ef_construction);
    hnsw_params.value->num_threads = int(config.cpu_threads);
    Handle<cuvsHnswIndex_t, cuvsHnswIndexCreate, cuvsHnswIndexDestroy> hnsw("cuvsHnswIndexCreate");
    cuvs_check(cuvsHnswFromCagraWithDataset(resources.value, hnsw_params.value, graph.value,
                                          hnsw.value, &dataset), "cuvsHnswFromCagraWithDataset");
    cuvs_check(cuvsStreamSync(resources.value), "cuvsStreamSync");
    hip_check(hipDeviceSynchronize());
    stats->synchronized = 1;
    TemporaryFile file;
    cuvs_check(cuvsHnswSerialize(resources.value, file.path.c_str(), hnsw.value), "cuvsHnswSerialize");
    std::ifstream input(file.path, std::ios::binary);
    require(input.is_open(), "cannot read exported full HNSW graph");
    char buffer[65536];
    uint64_t written = 0;
    while (input.read(buffer, sizeof(buffer)) || input.gcount() != 0) {
      const auto length = uint64_t(input.gcount());
      require(write(context, buffer, length) == 0, "HNSW output callback rejected export (size limit or memory)");
      written += length;
    }
    require(!input.bad() && written > 0, "failed to export full HNSW graph");
    stats->backend = SEEKDB_HIPVS_AMD_BACKEND;
    stats->bridge_wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return 0;
  } catch (const std::exception &exception) {
    if (error != nullptr && error_capacity != 0) {
      std::snprintf(error, size_t(error_capacity), "%s", exception.what());
    }
    return 1;
  } catch (...) {
    if (error != nullptr && error_capacity != 0) {
      std::snprintf(error, size_t(error_capacity), "unknown GPU build failure");
    }
    return 1;
  }
}
const SeekdbHipvsApi api{SEEKDB_HIPVS_ABI_VERSION, sizeof(SeekdbHipvsApi), build};
}

extern "C" __attribute__((visibility("default")))
const SeekdbHipvsApi *seekdb_hipvs_get_api_v1()
{
  return &api;
}
