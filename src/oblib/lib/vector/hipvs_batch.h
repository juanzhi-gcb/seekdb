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

#pragma once
#include "hipvs_bridge_abi.h"
#include "hipvs_hnsw_import.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oceanbase { namespace hipvs {

enum class BuildBackend { CPU, HIPVS };
BuildBackend selected_build_backend();

struct BuildOptions {
  uint64_t input_limit;
  uint64_t export_limit;
  std::string library_path;
  std::string worker_path;
  uint32_t worker_timeout_seconds;
  int cpu_threads;
  int device;
  // Read once when the first DDL row is buffered, and retain for finalization.
  static BuildOptions from_environment();
};

class GpuLibrary {
public:
  explicit GpuLibrary(const std::string &absolute_path, const std::string &worker_path = "",
                      uint32_t timeout_seconds = 1800);
  ~GpuLibrary();
  GpuLibrary(const GpuLibrary &) = delete;
  GpuLibrary &operator=(const GpuLibrary &) = delete;
  std::vector<uint8_t> build(const SeekdbHipvsBuildConfig &config, const float *vectors,
                             uint64_t export_limit, SeekdbHipvsBuildStats &stats) const;
private:
  std::string library_path_;
  std::string worker_path_;
  uint32_t timeout_seconds_;
};

// Externally synchronized by the seekdb snapshot handler. Owns all input data;
// DDL row arenas can therefore be reused immediately after append returns.
class BuildBatch {
public:
  BuildBatch(int dimension, uint64_t input_limit);
  void append(const float *vectors, const int64_t *ids, int dimension, int64_t count);
  void validate_ids() const;
  uint64_t count() const { return ids_.size(); }
  uint64_t reserved_bytes() const {
    return uint64_t(vectors_.capacity()) * sizeof(float) + uint64_t(ids_.capacity()) * sizeof(int64_t);
  }
  const std::vector<float> &vectors() const { return vectors_; }
  const std::vector<int64_t> &ids() const { return ids_; }
  std::shared_ptr<vsag::Index> finalize(
      const std::string &library_path, const ImportConfig &config,
      int ef_construction, int threads, int device, uint64_t export_limit,
      SeekdbHipvsBuildStats &stats, vsag::Allocator *allocator = nullptr,
      const std::string &worker_path = "", uint32_t timeout_seconds = 1800) const;
private:
  int dimension_;
  uint64_t input_limit_;
  std::vector<float> vectors_;
  std::vector<int64_t> ids_;
};

} }
