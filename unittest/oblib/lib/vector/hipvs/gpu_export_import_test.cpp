/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */

// Reads externally produced, real GPU exports. This test never builds a fixture.
#include "hipvs_hnsw_import.h"
#include <vsag/factory.h>
#include <vsag/filter.h>
#include <vsag/vsag.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
constexpr size_t count = 512;
constexpr size_t dim = 32;
constexpr size_t query_count = 64;
constexpr size_t k = 10;
void check(bool condition, const std::string &message)
{
  if (!condition) throw std::runtime_error(message);
}
std::vector<uint8_t> load(const std::string &path)
{
  std::ifstream input(path, std::ios::binary);
  check(input.good(), "cannot read " + path);
  return {(std::istreambuf_iterator<char>(input)), {}};
}
template<typename T> T at(const std::vector<uint8_t> &data, size_t offset)
{
  check(offset + sizeof(T) <= data.size(), "test parser outside file");
  T value;
  std::memcpy(&value, data.data() + offset, sizeof(T));
  return value;
}
double distance(const float *query, const float *point, bool ip)
{
  double value = 0;
  for (size_t d = 0; d < dim; ++d) {
    if (ip) value += double(query[d]) * double(point[d]);
    else {
      const double delta = double(query[d]) - double(point[d]);
      value += delta * delta;
    }
  }
  return ip ? 1.0 - value : value;
}
void check_distance(float actual, double expected, const std::string &context)
{
  const double tolerance = 3e-5 * std::max(1.0, std::abs(expected));
  check(std::isfinite(actual) && std::abs(double(actual) - expected) <= tolerance,
      context + ": actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
}
vsag::DatasetPtr dataset(const float *point)
{
  return vsag::Dataset::Make()->NumElements(1)->Dim(dim)->Float32Vectors(point)->Owner(false);
}
size_t ordinal(int64_t id)
{
  check(id >= 101 && (id - 101) % 17 == 0 && (id - 101) / 17 < int64_t(count),
      "ANN returned ID outside original mapped VIDs: " + std::to_string(id));
  return size_t((id - 101) / 17);
}
struct Search {
  std::vector<int64_t> ids;
  std::vector<float> distances;
};
class OddVidFilter final : public vsag::Filter {
 public:
  bool CheckValid(int64_t id) const override { return id % 2 == 1; }
  float ValidRatio() const override { return 0.5f; }
};
Search search(const std::shared_ptr<vsag::Index> &index, const float *query,
    const std::vector<float> &vectors, bool ip, size_t ef, bool filtered)
{
  const std::string parameters = "{\"hnsw\":{\"ef_search\":" + std::to_string(ef) + "}}";
  auto result = filtered ? index->KnnSearch(dataset(query), k, parameters,
      static_cast<vsag::FilterPtr>(std::make_shared<OddVidFilter>())) :
      index->KnnSearch(dataset(query), k, parameters);
  check(result.has_value(), "public KnnSearch failed");
  check(result.value()->GetDim() == int64_t(k), "ANN returned fewer than K neighbors");
  Search answer;
  std::set<int64_t> unique;
  for (size_t j = 0; j < k; ++j) {
    const auto id = result.value()->GetIds()[j];
    const auto row = ordinal(id);
    check(unique.insert(id).second, "ANN returned a repeated VID");
    check(!filtered || id % 2 == 1, "filter returned excluded VID");
    const auto value = result.value()->GetDistances()[j];
    check_distance(value, distance(query, vectors.data() + row * dim, ip), "ANN raw distance");
    answer.ids.push_back(id);
    answer.distances.push_back(value);
  }
  return answer;
}
std::vector<int64_t> truth(const float *query, const std::vector<float> &vectors, bool ip, bool filtered)
{
  std::vector<std::pair<double, int64_t>> candidates;
  for (size_t row = 0; row < count; ++row) {
    const int64_t id = 101 + 17 * row;
    if (!filtered || id % 2 == 1) candidates.emplace_back(
        distance(query, vectors.data() + row * dim, ip), id);
  }
  std::sort(candidates.begin(), candidates.end());
  std::vector<int64_t> result;
  for (size_t j = 0; j < k; ++j) result.push_back(candidates[j].second);
  return result;
}
void verify_all_vectors(const std::shared_ptr<vsag::Index> &index,
    const std::vector<float> &vectors, const std::vector<int64_t> &ids, bool ip)
{
  // Exercise every imported VID against every original FP32 row. Expected
  // distances accumulate in FP64; input data are never normalized or altered.
  for (size_t q = 0; q < count; ++q) {
    auto result = index->CalDistanceById(vectors.data() + q * dim, ids.data(), count);
    check(result.has_value(), "CalDistanceById failed for original mapped VIDs");
    check(result.value()->GetNumElements() == int64_t(count), "CalDistanceById lost rows");
    for (size_t row = 0; row < count; ++row) check_distance(result.value()->GetDistances()[row],
        distance(vectors.data() + q * dim, vectors.data() + row * dim, ip),
        "all-row CalDistanceById q=" + std::to_string(q) + " row=" + std::to_string(row));
  }
}
void run(const std::string &directory, const std::string &name, bool ip,
    const std::vector<float> &vectors, const std::vector<float> &queries)
{
  const auto bytes = load(directory + "/" + name);
  const std::string metric = ip ? "ip" : "l2";
  const std::string parameters = "{\"dtype\":\"float32\",\"metric_type\":\"" + metric +
      "\",\"dim\":32,\"use_old_serial_format\":true,\"hnsw\":{\"max_degree\":16,\"ef_construction\":200}}";
  const oceanbase::hipvs::ImportConfig config{int(dim), 16, metric, parameters};
  const auto info = oceanbase::hipvs::inspect_hnsw(bytes, config, count);
  check(info.count == count && info.max_level > 0, "full GPU graph hierarchy missing");
  const auto stride = at<uint64_t>(bytes, 24);
  const auto label_offset = at<uint64_t>(bytes, 32);
  const auto vector_offset = at<uint64_t>(bytes, 40);
  size_t shuffled = 0;
  for (size_t slot = 0; slot < count; ++slot) {
    const auto label = at<uint64_t>(bytes, 96 + slot * stride + label_offset);
    check(label < count, "GPU label out of bounds");
    check(std::memcmp(bytes.data() + 96 + slot * stride + vector_offset,
        vectors.data() + label * dim, dim * sizeof(float)) == 0,
        "GPU export changed original FP32 vector bytes at label " + std::to_string(label));
    shuffled += label != slot;
  }
  std::vector<int64_t> ids(count);
  for (size_t row = 0; row < count; ++row) ids[row] = 101 + 17 * row;
  std::cout << "START " << name << " max_level=" << info.max_level
      << " entrypoint=" << info.entrypoint << " shuffled_slots=" << shuffled << std::endl;
  auto index = oceanbase::hipvs::import_hnsw(bytes, ids, config);
  check(index->GetNumElements() == int64_t(count), "import lost vectors");
  verify_all_vectors(index, vectors, ids, ip);
  std::cout << "DISTANCE_PASS " << name << " comparisons=" << count * count << std::endl;
  std::stringstream stored(std::ios::in | std::ios::out | std::ios::binary);
  check(index->Serialize(stored).has_value(), "public VSAG Serialize failed");
  const auto serialized_bytes = stored.str().size();
  auto created = vsag::Factory::CreateIndex("hnsw", parameters);
  check(created.has_value(), "public VSAG restore CreateIndex failed");
  auto restored = created.value();
  check(restored->Deserialize(stored).has_value(), "public VSAG Deserialize failed");
  check(restored->GetNumElements() == int64_t(count), "restore lost vectors");
  verify_all_vectors(restored, vectors, ids, ip);
  for (const size_t ef : {size_t(128), count}) {
    for (const bool filtered : {false, true}) {
      size_t hits = 0;
      for (size_t q = 0; q < query_count; ++q) {
        const auto *query = queries.data() + q * dim;
        const auto exact = truth(query, vectors, ip, filtered);
        const auto original = search(index, query, vectors, ip, ef, filtered);
        const auto reloaded = search(restored, query, vectors, ip, ef, filtered);
        check(original.ids == reloaded.ids && original.distances == reloaded.distances,
            "Serialize/Deserialize changed ANN IDs or raw distances");
        for (const auto id : original.ids) hits += std::find(exact.begin(), exact.end(), id) != exact.end();
      }
      std::cout << "RESULT_JSON {\"file\":\"" << name << "\",\"metric\":\"" << metric
          << "\",\"ef_search\":" << ef << ",\"filtered\":" << (filtered ? "true" : "false")
          << ",\"queries\":" << query_count << ",\"k\":" << k << ",\"exact_hits\":" << hits
          << ",\"recall_at_10\":" << std::setprecision(10) << double(hits) / (query_count * k)
          << ",\"serialization_identical\":true,\"serialized_bytes\":" << serialized_bytes
          << ",\"shuffled_slots\":" << shuffled << "}" << std::endl;
    }
  }
  const int64_t added_id = 900001;
  std::vector<float> added(dim);
  for (size_t d = 0; d < dim; ++d) added[d] = float(3.25 + d * 0.03125);
  auto add = dataset(added.data())->Ids(&added_id);
  const auto added_result = restored->Add(add);
  check(added_result.has_value() && added_result.value().empty(), "online CPU Add rejected new VID");
  check(restored->GetNumElements() == int64_t(count + 1), "online Add count mismatch");
  auto new_distance = restored->CalDistanceById(queries.data(), &added_id, 1);
  check(new_distance.has_value(), "online added VID not accessible");
  check_distance(new_distance.value()->GetDistances()[0], distance(queries.data(), added.data(), ip),
      "online Add preserves raw FP32 distance");
  auto found = restored->KnnSearch(dataset(added.data()), k, R"({"hnsw":{"ef_search":512}})");
  check(found.has_value() && found.value()->GetDim() == int64_t(k), "online Add query failed");
  bool contains_added = false;
  std::set<int64_t> seen;
  for (size_t j = 0; j < k; ++j) {
    const auto id = found.value()->GetIds()[j];
    check(seen.insert(id).second, "online Add search returned duplicate VID");
    const float *point = id == added_id ? added.data() : vectors.data() + ordinal(id) * dim;
    contains_added |= id == added_id;
    check_distance(found.value()->GetDistances()[j], distance(added.data(), point, ip), "online Add ANN distance");
  }
  // The inserted point need not be its own IP nearest neighbor.
  check(ip || contains_added, "online Add L2 self-query failed to reach inserted point");
  std::cout << "PASS " << name << " original_fp32=bitwise_equal full_mapped_distances=verified"
      << " persistence=identical filter=verified online_add=verified" << std::endl;
}
}

int main(int argc, char **argv)
{
  try {
    check(argc == 2, "usage: gpu_export_import_test GPU_EXPORT_DIRECTORY");
    std::cout << "VSAG SDK revision: " << vsag::version() << std::endl;
    const std::string directory = argv[1];
    const auto raw = load(directory + "/vectors.f32");
    check(raw.size() == count * dim * sizeof(float), "original GPU input shape must be 512x32 FP32");
    std::vector<float> vectors(count * dim);
    std::memcpy(vectors.data(), raw.data(), raw.size());
    // Independent deterministic probes, never original rows or normalized data.
    std::mt19937 generator(20261027);
    std::vector<float> queries(query_count * dim);
    for (auto &value : queries) value = float(int64_t(generator()) - int64_t(2147483648ULL)) / 1073741824.0f;
    size_t failures = 0;
    for (int metric = 1; metric <= 2; ++metric) {
      for (int repetition = 0; repetition < 3; ++repetition) {
        const auto name = "metric-" + std::to_string(metric) + "-run-" + std::to_string(repetition) + ".hnsw";
        try { run(directory, name, metric == 2, vectors, queries); }
        catch (const std::exception &error) {
          ++failures;
          std::cerr << "FAIL " << name << ": " << error.what() << std::endl;
        }
      }
    }
    check(failures == 0, std::to_string(failures) + " real GPU export cases failed");
    std::cout << "PASS_ALL: six real GPU exports; Recall reported without a quality acceptance threshold; SQL not tested" << std::endl;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return 1;
  }
}
