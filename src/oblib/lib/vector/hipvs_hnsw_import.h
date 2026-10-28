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

#include <vsag/index.h>
#include <vsag/allocator.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oceanbase { namespace hipvs {

struct ImportConfig {
  int dim;
  int max_degree;
  std::string metric;
  std::string vsag_parameters;
};

struct HnswFileInfo {
  uint64_t count;
  int32_t max_level;
  uint32_t entrypoint;
};

// Accept only the verified x86_64 little-endian, FP32, full hnswlib layout.
// Throws before VSAG allocation if the complete graph/labels/layout are invalid.
HnswFileInfo inspect_hnsw(const std::vector<uint8_t> &bytes,
                         const ImportConfig &config, uint64_t expected_count);

// No private VSAG symbols and no modification of serialized graph bytes.
// Dense hnswlib labels are mapped through public UpdateId in two collision-free passes.
std::shared_ptr<vsag::Index> import_hnsw(
    const std::vector<uint8_t> &bytes, const std::vector<int64_t> &vids,
    const ImportConfig &config, vsag::Allocator *allocator = nullptr);

} }
