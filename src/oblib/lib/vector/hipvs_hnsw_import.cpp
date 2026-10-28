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

#include "hipvs_hnsw_import.h"
#include <vsag/factory.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace oceanbase { namespace hipvs {
namespace {
void import_require(bool condition, const char *message)
{
  if (!condition) throw std::invalid_argument(message);
}

template <typename T> T read(const std::vector<uint8_t> &bytes, size_t &position)
{
  import_require(position <= bytes.size() && sizeof(T) <= bytes.size() - position,
          "hipVS HNSW file is truncated");
  T value;
  std::memcpy(&value, bytes.data() + position, sizeof(T));
  position += sizeof(T);
  return value;
}

template <typename T> T at(const std::vector<uint8_t> &bytes, size_t position)
{
  return read<T>(bytes, position);
}

void validate_links(const std::vector<uint8_t> &bytes, size_t position,
                    uint64_t capacity, uint64_t count, uint32_t node,
                    const std::vector<uint32_t> *levels = nullptr, uint32_t level = 0)
{
  const auto size = read<uint32_t>(bytes, position);
  import_require(size <= capacity, "invalid HNSW adjacency count or deleted flag");
  std::unordered_set<uint32_t> neighbors;
  for (uint32_t j = 0; j < size; ++j) {
    const auto neighbor = read<uint32_t>(bytes, position);
    import_require(neighbor < count && neighbor != node, "invalid HNSW neighbor ordinal");
    import_require(neighbors.insert(neighbor).second, "duplicate HNSW edge");
    if (levels != nullptr) import_require((*levels)[neighbor] >= level, "HNSW upper edge leaves its layer");
  }
}

void remap(vsag::Index &index, int64_t old_id, int64_t new_id)
{
  const auto result = index.UpdateId(old_id, new_id);
  if (!result.has_value() || !result.value()) {
    throw std::runtime_error("VSAG failed to map a hipVS ordinal to its seekdb VID");
  }
}
}

HnswFileInfo inspect_hnsw(const std::vector<uint8_t> &bytes,
                         const ImportConfig &config, uint64_t expected_count)
{
  const uint16_t endian = 1;
  import_require(sizeof(size_t) == 8 && *reinterpret_cast<const uint8_t *>(&endian) == 1,
          "hipVS HNSW import requires a 64-bit little-endian host");
  import_require(config.dim > 0 && config.max_degree >= 8 && config.max_degree <= 64,
          "unsupported HNSW dimension or degree (VSAG requires M in [8, 64])");
  import_require(config.metric == "l2" || config.metric == "ip", "only FP32 L2/IP HNSW import is verified");
  import_require(expected_count > 0 && expected_count <= std::numeric_limits<uint32_t>::max(),
          "invalid HNSW vector count");
  size_t cursor = 0;
  const auto level0_offset = read<uint64_t>(bytes, cursor);
  const auto capacity = read<uint64_t>(bytes, cursor);
  const auto count = read<uint64_t>(bytes, cursor);
  const auto stride = read<uint64_t>(bytes, cursor);
  const auto label_offset = read<uint64_t>(bytes, cursor);
  const auto data_offset = read<uint64_t>(bytes, cursor);
  const auto max_level = read<int32_t>(bytes, cursor);
  const auto entrypoint = read<uint32_t>(bytes, cursor);
  const auto max_m = read<uint64_t>(bytes, cursor);
  const auto max_m0 = read<uint64_t>(bytes, cursor);
  const auto m = read<uint64_t>(bytes, cursor);
  const auto multiplier = read<double>(bytes, cursor);
  const auto ef_construction = read<uint64_t>(bytes, cursor);
  import_require(cursor == 96 && count == expected_count, "HNSW layout/count mismatch");
  // The hipVS full-index converter allocates exactly N nodes. Reject inflated
  // capacities before handing the file to a library that allocates from its header.
  import_require(capacity == count, "hipVS export must have an exact, bounded capacity");
  import_require(max_m == uint64_t(config.max_degree) && m == max_m && max_m0 == 2 * m,
          "HNSW degree does not match seekdb creation parameters");
  import_require(level0_offset == 0 && data_offset == 4 + max_m0 * 4,
          "HNSW level-zero layout mismatch");
  import_require(label_offset == data_offset + uint64_t(config.dim) * 4 && stride == label_offset + 8,
          "HNSW FP32 vector/label layout mismatch (extra information is unsupported)");
  import_require(max_level >= 0 && max_level < 64 && entrypoint < count &&
          std::isfinite(multiplier) && multiplier > 0 && ef_construction >= m,
          "invalid HNSW hierarchy header");
  import_require(stride > 0 && count <= (bytes.size() - cursor) / stride, "truncated HNSW level zero");
  const size_t level0_start = cursor;
  std::vector<bool> labels(count, false);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t row = level0_start + size_t(i) * stride;
    validate_links(bytes, row, max_m0, count, i);
    const auto label = at<uint64_t>(bytes, row + label_offset);
    import_require(label < count && !labels[label], "hipVS labels must be a permutation of dense ordinals");
    labels[label] = true;
    for (int d = 0; d < config.dim; ++d) {
      import_require(std::isfinite(at<float>(bytes, row + data_offset + size_t(d) * 4)), "non-finite HNSW vector");
    }
  }
  cursor += count * stride;
  const size_t upper_stride = 4 + max_m * 4;
  std::vector<uint32_t> levels(count);
  std::vector<size_t> upper_offsets(count);
  uint32_t actual_max_level = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const auto length = read<uint32_t>(bytes, cursor);
    import_require(length % upper_stride == 0 && length / upper_stride < 64, "invalid HNSW upper-layer length");
    import_require(length <= bytes.size() - cursor, "truncated HNSW upper layers");
    levels[i] = length / upper_stride;
    upper_offsets[i] = cursor;
    actual_max_level = std::max(actual_max_level, levels[i]);
    cursor += length;
  }
  import_require(cursor == bytes.size(), "unexpected bytes following full HNSW graph");
  import_require(actual_max_level == uint32_t(max_level) && levels[entrypoint] == actual_max_level,
          "HNSW entrypoint/max-level mismatch");
  for (uint32_t i = 0; i < count; ++i) {
    for (uint32_t level = 1; level <= levels[i]; ++level) {
      validate_links(bytes, upper_offsets[i] + size_t(level - 1) * upper_stride,
                     max_m, count, i, &levels, level);
    }
  }
  return {count, max_level, entrypoint};
}

std::shared_ptr<vsag::Index> import_hnsw(
    const std::vector<uint8_t> &bytes, const std::vector<int64_t> &vids,
    const ImportConfig &config, vsag::Allocator *allocator)
{
  inspect_hnsw(bytes, config, vids.size());
  std::unordered_set<int64_t> unique;
  for (const auto vid : vids) {
    import_require(vid >= 0, "seekdb VID must be nonnegative for collision-free import");
    import_require(unique.insert(vid).second, "duplicate seekdb VID in GPU batch");
  }
  auto created = vsag::Factory::CreateIndex("hnsw", config.vsag_parameters, allocator);
  if (!created.has_value()) throw std::runtime_error("VSAG rejected hipVS import parameters: " + created.error().message);
  auto index = created.value();
  vsag::Binary binary;
  binary.size = bytes.size();
  binary.data.reset(new int8_t[bytes.size()]);
  std::memcpy(binary.data.get(), bytes.data(), bytes.size());
  vsag::BinarySet set;
  set.Set("hnsw_data", binary);
  const auto imported = index->Deserialize(set);
  if (!imported.has_value()) throw std::runtime_error("VSAG could not import full hipVS HNSW: " + imported.error().message);
  import_require(index->GetNumElements() == int64_t(vids.size()), "VSAG import lost vectors");
  for (size_t i = 0; i < vids.size(); ++i) remap(*index, int64_t(i), -int64_t(i) - 1);
  for (size_t i = 0; i < vids.size(); ++i) remap(*index, -int64_t(i) - 1, vids[i]);
  return index;
}

} }
