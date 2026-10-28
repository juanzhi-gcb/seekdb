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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEEKDB_HIPVS_ABI_VERSION 1u
#define SEEKDB_HIPVS_AMD_BACKEND 1u
#define SEEKDB_HIPVS_L2 1u
#define SEEKDB_HIPVS_IP 2u

struct SeekdbHipvsBuildConfig {
  uint32_t size;
  uint32_t dimension;
  uint64_t rows;
  uint32_t max_degree;
  uint32_t ef_construction;
  uint32_t metric;
  uint32_t cpu_threads;
  int32_t device;
  uint32_t reserved;
};

struct SeekdbHipvsBuildStats {
  uint32_t size;
  uint32_t backend;
  uint32_t gpu_build_submissions;
  uint32_t synchronized;
  double gpu_build_ms;
  double bridge_wall_ms;
  char device_name[128];
  char version[128];
};

// The callback copies a serialized FULL HNSW graph. Return nonzero to abort.
typedef int (*SeekdbHipvsWrite)(void *context, const void *data, uint64_t length);
typedef int (*SeekdbHipvsBuild)(const struct SeekdbHipvsBuildConfig *config,
                               const float *vectors, SeekdbHipvsWrite write,
                               void *context, struct SeekdbHipvsBuildStats *stats,
                               char *error, uint64_t error_capacity);
struct SeekdbHipvsApi {
  uint32_t version;
  uint32_t size;
  SeekdbHipvsBuild build;
};
typedef const struct SeekdbHipvsApi *(*SeekdbHipvsGetApi)(void);

#ifdef __cplusplus
}
#endif

// Private Linux little-endian IPC protocol. This does not change bridge ABI v1.
// All fields have fixed widths; parent and optional worker are built together.
#define SEEKDB_HIPVS_WORKER_PROTOCOL_VERSION 1u
#define SEEKDB_HIPVS_WORKER_REQUEST_MAGIC UINT64_C(0x3151455253565048)
#define SEEKDB_HIPVS_WORKER_RESPONSE_MAGIC UINT64_C(0x3150535253565048)
struct SeekdbHipvsWorkerRequest {
  uint64_t magic;
  uint32_t version;
  uint32_t header_size;
  struct SeekdbHipvsBuildConfig config;
  uint64_t input_bytes;
  uint64_t max_export_bytes;
};
struct SeekdbHipvsWorkerResponse {
  uint64_t magic;
  uint32_t version;
  uint32_t header_size;
  uint32_t status;
  uint32_t reserved;
  uint64_t output_bytes;
  struct SeekdbHipvsBuildStats stats;
  char error[1024];
};
#ifdef __cplusplus
static_assert(sizeof(SeekdbHipvsWorkerRequest) == 72, "hipVS worker request layout");
static_assert(sizeof(SeekdbHipvsWorkerResponse) == 1344, "hipVS worker response layout");
#endif
