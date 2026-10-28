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
#include <hnswlib/hnswlib.h>
#include <vsag/factory.h>
#include <vsag/filter.h>
#include <vsag/vsag.h>
#include <random>
#include <algorithm>
#include <limits>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <unistd.h>

namespace {
void check(bool value, const char *message)
{
  if (!value) throw std::runtime_error(message);
}

const std::string parameters = R"({"dtype":"float32","metric_type":"l2","dim":4,"use_old_serial_format":true,"hnsw":{"max_degree":16,"ef_construction":64,"ef_search":128}})";

std::vector<uint8_t> native_fixture(const std::vector<float> &vectors, bool inner_product = false,
    std::vector<std::pair<int64_t, float>> *native_ip_results = nullptr)
{
  // This CPU-generated fixture tests format compatibility, not GPU execution.
  hnswlib::L2Space l2_space(4);
  hnswlib::InnerProductSpace ip_space(4);
  hnswlib::SpaceInterface<float> *space = inner_product ?
      static_cast<hnswlib::SpaceInterface<float> *>(&ip_space) : &l2_space;
  const size_t count = vectors.size() / 4;
  hnswlib::HierarchicalNSW<float> index(space, count, 16, 64, 100);
  // For this power-of-two fixture, 73 is coprime to count: internal node IDs
  // differ from the external ordinal labels used by the production bridge.
  for (size_t inserted = 0; inserted < count; ++inserted) {
    const size_t ordinal = (inserted * 73 + 19) % count;
    index.addPoint(vectors.data() + ordinal * 4, ordinal);
  }
  check(index.getExternalLabel(0) == 19, "fixture insertion order differs from ordinal labels");
  if (inner_product && native_ip_results != nullptr) {
    index.setEf(256);
    for (size_t q = 0; q < count; q += 13) {
      const float probe[] = {float(std::cos(q * 0.23)), float(std::sin(q * 0.11)),
          float(std::cos(q * 0.37)), float(std::sin(q * 0.41))};
      const auto native = index.searchKnn(probe, 1);
      native_ip_results->emplace_back(native.top().second, native.top().first);
    }
  }
  char name[] = "/tmp/seekdb-hipvs-fixture-XXXXXX";
  const int fd = mkstemp(name);
  check(fd >= 0, "create fixture file");
  close(fd);
  index.saveIndex(name);
  std::ifstream in(name, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
  std::remove(name);
  return bytes;
}

void assert_self_queries(const std::shared_ptr<vsag::Index> &index,
                         const std::vector<float> &vectors,
                         const std::vector<int64_t> &ids)
{
  for (size_t i = 0; i < ids.size(); i += 7) {
    auto query = vsag::Dataset::Make()->NumElements(1)->Dim(4)
        ->Float32Vectors(vectors.data() + i * 4)->Owner(false);
    auto result = index->KnnSearch(query, 1, R"({"hnsw":{"ef_search":128}})");
    check(result.has_value(), "query imported HNSW");
    check(result.value()->GetDim() == 1, "one nearest neighbor");
    check(result.value()->GetIds()[0] == ids[i], "original noncontinuous VID");
    check(std::abs(result.value()->GetDistances()[0]) < 1e-5, "exact self distance");
  }
}

class OddVidFilter final : public vsag::Filter {
public:
  bool CheckValid(int64_t id) const override { return id % 2 != 0; }
  float ValidRatio() const override { return 0.5f; }
};

void assert_filtered_queries(const std::shared_ptr<vsag::Index> &index,
                             const std::vector<float> &vectors,
                             const std::vector<int64_t> &ids)
{
  auto filter = std::make_shared<OddVidFilter>();
  for (size_t q = 0; q < ids.size(); q += 19) {
    size_t nearest = ids.size();
    float best_distance = std::numeric_limits<float>::max();
    for (size_t row = 0; row < ids.size(); ++row) {
      if (!filter->CheckValid(ids[row])) continue;
      float distance = 0;
      for (int d = 0; d < 4; ++d) {
        const float delta = vectors[q * 4 + d] - vectors[row * 4 + d];
        distance += delta * delta;
      }
      if (distance < best_distance) { best_distance = distance; nearest = row; }
    }
    auto query = vsag::Dataset::Make()->NumElements(1)->Dim(4)
        ->Float32Vectors(vectors.data() + q * 4)->Owner(false);
    auto found = index->KnnSearch(query, 1, R"({"hnsw":{"ef_search":256}})",
        static_cast<vsag::FilterPtr>(filter));
    check(found.has_value() && found.value()->GetDim() == 1, "filtered imported query returns a neighbor");
    check(found.value()->GetIds()[0] == ids[nearest], "filtered nearest VID matches brute force");
    check(std::abs(found.value()->GetDistances()[0] - best_distance) < 1e-4, "filtered distance matches brute force");
  }
}

void assert_ip_queries(const std::vector<float> &vectors, const std::vector<int64_t> &ids, bool require_exact_truth)
{
  std::vector<std::pair<int64_t, float>> native_results;
  const auto bytes = native_fixture(vectors, true, &native_results);
  std::string ip_parameters = parameters;
  ip_parameters.replace(ip_parameters.find("\"l2\""), 4, "\"ip\"");
  const oceanbase::hipvs::ImportConfig config{4, 16, "ip", ip_parameters};
  auto index = oceanbase::hipvs::import_hnsw(bytes, ids, config);
  size_t native_ann_misses = 0;
  for (size_t q = 0; q < ids.size(); q += 13) {
    const float query_vector[] = {float(std::cos(q * 0.23)), float(std::sin(q * 0.11)),
        float(std::cos(q * 0.37)), float(std::sin(q * 0.41))};
    size_t nearest = 0;
    float best_dot = std::numeric_limits<float>::lowest();
    for (size_t row = 0; row < ids.size(); ++row) {
      float dot = 0;
      for (int d = 0; d < 4; ++d) dot += query_vector[d] * vectors[row * 4 + d];
      if (dot > best_dot) { best_dot = dot; nearest = row; }
    }
    auto exact_distances = index->CalDistanceById(query_vector, ids.data(), ids.size());
    check(exact_distances.has_value(), "IP imported distances by original VIDs");
    for (size_t row = 0; row < ids.size(); ++row) {
      float dot = 0;
      for (int d = 0; d < 4; ++d) dot += query_vector[d] * vectors[row * 4 + d];
      check(std::abs(exact_distances.value()->GetDistances()[row] - (1 - dot)) < 1e-4,
          "every imported IP vector and mapped VID preserves raw dot-product distance");
    }
    auto query = vsag::Dataset::Make()->NumElements(1)->Dim(4)->Float32Vectors(query_vector)->Owner(false);
    auto found = index->KnnSearch(query, 1, R"({"hnsw":{"ef_search":256}})");
    check(found.has_value() && found.value()->GetDim() == 1, "IP imported query returns a neighbor");
    const auto native = native_results[q / 13];
    check(found.value()->GetIds()[0] == ids[native.first], "imported IP ANN result agrees with original native graph");
    check(std::abs(found.value()->GetDistances()[0] - native.second) < 1e-4, "imported IP ANN distance agrees with native");
    if (ids[native.first] != ids[nearest]) ++native_ann_misses;
    if (require_exact_truth) {
      check(found.value()->GetIds()[0] == ids[nearest], "well-conditioned IP nearest VID matches brute-force dot product");
      check(std::abs(found.value()->GetDistances()[0] - (1 - best_dot)) < 1e-4,
          "well-conditioned IP nearest distance matches brute-force dot product");
    }
  }
  if (!require_exact_truth) {
    // Nonmetric IP can leave nodes unreachable even at ef=N. Preserve this
    // observed native behavior; the importer must neither hide nor worsen it.
    std::cout << "Observed native IP ANN misses against exact truth: " << native_ann_misses
        << " (imported results match native; not a Recall acceptance claim)\n";
  }
}

template <typename F> void rejects(F action, const char *message)
{
  bool rejected = false;
  try { action(); } catch (const std::exception &) { rejected = true; }
  check(rejected, message);
}
}

int main()
{
  try {
    std::cout << "VSAG SDK revision: " << vsag::version() << '\n';
    std::vector<float> vectors;
    std::vector<int64_t> ids;
    for (int64_t i = 0; i < 256; ++i) {
      ids.push_back(10001 + i * 17);
      for (int d = 0; d < 4; ++d) vectors.push_back(float(std::sin(i * 0.31 + d) + i * 0.01));
    }
    const auto bytes = native_fixture(vectors);
    oceanbase::hipvs::ImportConfig config{4, 16, "l2", parameters};
    const auto info = oceanbase::hipvs::inspect_hnsw(bytes, config, ids.size());
    check(info.max_level > 0, "fixture preserves a real multi-level graph");
    auto index = oceanbase::hipvs::import_hnsw(bytes, ids, config);
    check(index->GetNumElements() == 256, "all vectors imported");
    assert_self_queries(index, vectors, ids);
    assert_filtered_queries(index, vectors, ids);

    // Final VIDs deliberately collide with every original dense ordinal. A
    // one-pass UpdateId would overwrite existing labels or fail on this cycle.
    std::vector<int64_t> cyclic_ids;
    for (size_t i = 0; i < ids.size(); ++i) cyclic_ids.push_back((i + 1) % ids.size());
    auto cyclic_index = oceanbase::hipvs::import_hnsw(bytes, cyclic_ids, config);
    assert_self_queries(cyclic_index, vectors, cyclic_ids);
    assert_filtered_queries(cyclic_index, vectors, cyclic_ids);
    assert_ip_queries(vectors, cyclic_ids, false);
    std::vector<float> spherical_vectors;
    std::mt19937 generator(20261026);
    std::normal_distribution<float> normal(0.0f, 1.0f);
    for (size_t row = 0; row < ids.size(); ++row) {
      float point[4];
      float norm = 0;
      for (auto &value : point) { value = normal(generator); norm += value * value; }
      for (const auto value : point) spherical_vectors.push_back(value / std::sqrt(norm));
    }
    // Equal norm is a test fixture property. The production path never normalizes IP input.
    assert_ip_queries(spherical_vectors, cyclic_ids, true);

    std::stringstream stored(std::ios::in | std::ios::out | std::ios::binary);
    check(index->Serialize(stored).has_value(), "VSAG serializes imported graph");
    auto restored = vsag::Factory::CreateIndex("hnsw", parameters).value();
    check(restored->Deserialize(stored).has_value(), "VSAG reloads its own serialized graph");
    assert_self_queries(restored, vectors, ids);
    assert_filtered_queries(restored, vectors, ids);

    const int64_t added_id = 900001;
    const float added_vector[] = {1000, 1001, 1002, 1003};
    auto add = vsag::Dataset::Make()->NumElements(1)->Dim(4)->Ids(&added_id)
        ->Float32Vectors(added_vector)->Owner(false);
    check(restored->Add(add).has_value(), "CPU incremental Add after GPU-compatible import");
    check(restored->GetNumElements() == 257, "incremental count");
    auto result = restored->KnnSearch(add, 1, R"({"hnsw":{"ef_search":128}})");
    check(result.has_value() && result.value()->GetIds()[0] == added_id, "incremental VID query");

    auto duplicate_ids = ids;
    duplicate_ids[1] = duplicate_ids[0];
    rejects([&] { oceanbase::hipvs::import_hnsw(bytes, duplicate_ids, config); }, "duplicate IDs rejected");
    auto negative_ids = ids;
    negative_ids[0] = -5;
    rejects([&] { oceanbase::hipvs::import_hnsw(bytes, negative_ids, config); }, "negative VID rejected");
    auto truncated = bytes;
    truncated.pop_back();
    rejects([&] { oceanbase::hipvs::inspect_hnsw(truncated, config, ids.size()); }, "truncated upper graph rejected");
    auto wrong_config = config;
    wrong_config.dim = 5;
    rejects([&] { oceanbase::hipvs::inspect_hnsw(bytes, wrong_config, ids.size()); }, "dimension mismatch rejected");
    wrong_config = config;
    wrong_config.metric = "cosine";
    rejects([&] { oceanbase::hipvs::inspect_hnsw(bytes, wrong_config, ids.size()); }, "unsupported normalization rejected");
    rejects([&] { oceanbase::hipvs::inspect_hnsw(bytes, config, ids.size() - 1); }, "missing vectors rejected");
    auto trailing = bytes;
    trailing.push_back(0);
    rejects([&] { oceanbase::hipvs::inspect_hnsw(trailing, config, ids.size()); }, "trailing data rejected");
    std::cout << "PASS: shuffled HNSW labels, colliding VID cycles, L2/IP brute-force truth, VSAG filters, persistence, incremental Add and rejection\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
