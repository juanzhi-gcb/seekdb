/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// Real GPU construction and public VSAG import execute in this single process.
#include "hipvs_batch.h"
#include <vsag/factory.h>
#include <vsag/vsag.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <link.h>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
constexpr size_t n = 512, dim = 32, nq = 64, k = 10;
void check(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
double distance(const float *a, const float *b, bool ip) {
  double result = 0;
  for (size_t d = 0; d < dim; ++d) {
    if (ip) result += double(a[d]) * double(b[d]);
    else { const double delta = double(a[d]) - double(b[d]); result += delta * delta; }
  }
  return ip ? 1.0 - result : result;
}
void check_distance(float actual, double expected) {
  check(std::isfinite(actual) && std::abs(double(actual) - expected) <=
      3e-5 * std::max(1.0, std::abs(expected)), "actual distance differs from original FP32");
}
vsag::DatasetPtr query(const float *point) {
  return vsag::Dataset::Make()->NumElements(1)->Dim(dim)->Float32Vectors(point)->Owner(false);
}
int libraries(dl_phdr_info *info, size_t, void *context) {
  const std::string path = info->dlpi_name;
  if (path.find("vsag") != std::string::npos || path.find("cuvs") != std::string::npos ||
      path.find("libstdc++") != std::string::npos || path.find("amdhip") != std::string::npos ||
      path.find("hipvs_bridge") != std::string::npos) {
    std::cout << "LIBMAP phase=" << *static_cast<const std::string *>(context) << " path=" << path << '\n';
  }
  return 0;
}
void snapshot(const std::string &phase) {
  dl_iterate_phdr(libraries, const_cast<std::string *>(&phase));
  std::cout.flush();
}
std::string parameters(bool ip) {
  return std::string("{\"dtype\":\"float32\",\"metric_type\":\"") + (ip ? "ip" : "l2") +
      "\",\"dim\":32,\"use_old_serial_format\":true,\"hnsw\":{\"max_degree\":16,\"ef_construction\":200}}";
}
void verify_distances(const std::shared_ptr<vsag::Index> &index, const std::vector<float> &vectors,
    const std::vector<int64_t> &ids, bool ip) {
  check(index->GetNumElements() == int64_t(n), "imported element count is not 512");
  for (size_t q = 0; q < n; ++q) {
    auto result = index->CalDistanceById(vectors.data() + q * dim, ids.data(), n);
    check(result.has_value() && result.value()->GetNumElements() == int64_t(n),
        "CalDistanceById lost mapped VIDs");
    for (size_t row = 0; row < n; ++row) check_distance(result.value()->GetDistances()[row],
        distance(vectors.data() + q * dim, vectors.data() + row * dim, ip));
  }
}
struct Answer { std::vector<int64_t> ids; std::vector<float> distances; };
Answer search(const std::shared_ptr<vsag::Index> &index, const float *probe,
    const std::vector<float> &vectors, bool ip) {
  auto found = index->KnnSearch(query(probe), k, R"({"hnsw":{"ef_search":128}})");
  check(found.has_value() && found.value()->GetDim() == int64_t(k), "KnnSearch did not return K");
  Answer answer;
  std::set<int64_t> seen;
  for (size_t j = 0; j < k; ++j) {
    const int64_t id = found.value()->GetIds()[j];
    check(id >= 101 && (id - 101) % 17 == 0 && (id - 101) / 17 < int64_t(n),
        "ANN ID is outside original mapped VIDs");
    check(seen.insert(id).second, "ANN ID is repeated");
    const auto row = size_t((id - 101) / 17);
    check_distance(found.value()->GetDistances()[j], distance(probe, vectors.data() + row * dim, ip));
    answer.ids.push_back(id);
    answer.distances.push_back(found.value()->GetDistances()[j]);
  }
  return answer;
}
template <typename F> void rejects(F action, const std::string &expected) {
  bool rejected = false;
  try { action(); } catch (const std::exception &error) {
    std::cout << "EXPECTED_REJECTION " << error.what() << std::endl;
    rejected = std::string(error.what()).find(expected) != std::string::npos;
  }
  check(rejected, "GPU request did not fail explicitly: " + expected);
}
}
int main(int argc, char **argv) {
  try {
    check(argc == 2, "usage: gpu_batch_smoke ABSOLUTE_BRIDGE_PATH");
    std::cout << "VSAG_REVISION " << vsag::version() << std::endl;
    std::vector<float> vectors(n * dim), probes(nq * dim);
    std::vector<int64_t> ids(n);
    std::mt19937 rng(20261027);
    for (size_t i = 0; i < n; ++i) {
      ids[i] = 101 + 17 * i;
      for (size_t d = 0; d < dim; ++d)
        vectors[i * dim + d] = float(std::sin(double(i) * 0.17 + double(d) * 0.31) +
            std::cos(double(i) * 0.07 - double(d) * 0.23));
    }
    for (auto &value : probes)
      value = float(int64_t(rng()) - int64_t(2147483648ULL)) / 1073741824.0f;
    // A live CPU VSAG index precedes the first bridge dlopen. Its data and
    // allocator remain active across all GPU builds and bridge destructors.
    auto sentinel_created = vsag::Factory::CreateIndex("hnsw", parameters(false));
    check(sentinel_created.has_value(), "VSAG sentinel CreateIndex failed");
    auto sentinel = sentinel_created.value();
    const int64_t sentinel_id = 991001;
    auto sentinel_data = query(vectors.data())->Ids(&sentinel_id);
    check(sentinel->Add(sentinel_data).has_value(), "VSAG sentinel Add failed");
    auto verify_sentinel = [&] {
      auto found = sentinel->KnnSearch(query(vectors.data()), 1, R"({"hnsw":{"ef_search":128}})");
      check(found.has_value() && found.value()->GetDim() == 1 &&
          found.value()->GetIds()[0] == sentinel_id, "live VSAG index corrupted across bridge lifecycle");
      check_distance(found.value()->GetDistances()[0], 0.0);
    };
    verify_sentinel();
    snapshot("vsag_live_before_bridge");
    oceanbase::hipvs::ImportConfig l2{int(dim), 16, "l2", parameters(false)};
    oceanbase::hipvs::BuildBatch small(dim, 1ULL << 20);
    small.append(vectors.data(), ids.data(), dim, 64);
    SeekdbHipvsBuildStats stats{};
    if (std::getenv("HIPVS_SMOKE_SKIP_NEGATIVE") == nullptr) {
    rejects([&] { small.finalize("/no/such/seekdb-hipvs-bridge.so", l2, 200, 4, 0,
        1ULL << 24, stats); }, "cannot load requested hipVS bridge");
    rejects([&] { small.finalize(argv[1], l2, 200, 4, 0, 1ULL << 24, stats); }, "no CPU fallback");
    check(stats.backend == 0 && stats.gpu_build_submissions == 0 && stats.synchronized == 0,
        "rejected small GPU request claimed successful build stats");
    snapshot("after_expected_failure_dlclose");
    }
    std::vector<std::shared_ptr<vsag::Index>> retained;
    for (int metric = 0; metric < 2; ++metric) {
      const bool ip = metric == 1;
      const std::string metric_name = ip ? "ip" : "l2";
      for (int repetition = 0; repetition < 2; ++repetition) {
        oceanbase::hipvs::BuildBatch batch(dim, 1ULL << 20);
        // Exercise owned, multiple append batches used by the production path.
        for (size_t start = 0; start < n; start += 64)
          batch.append(vectors.data() + start * dim, ids.data() + start, dim, 64);
        const oceanbase::hipvs::ImportConfig config{int(dim), 16, metric_name, parameters(ip)};
        std::cout << "START metric=" << metric_name << " repetition=" << repetition << std::endl;
        stats = {};
        auto index = batch.finalize(argv[1], config, 200, 4, 0, 1ULL << 24, stats);
        snapshot(metric_name + "_" + std::to_string(repetition) + "_after_finalize_dlclose");
        check(stats.backend == SEEKDB_HIPVS_AMD_BACKEND && stats.gpu_build_submissions == 1 &&
            stats.synchronized == 1 && stats.gpu_build_ms > 0 && stats.bridge_wall_ms > 0,
            "successful BuildBatch did not report exactly one synchronized real GPU build");
        verify_sentinel();
        verify_distances(index, vectors, ids, ip);
        std::stringstream storage(std::ios::in | std::ios::out | std::ios::binary);
        check(index->Serialize(storage).has_value(), "VSAG Serialize failed");
        auto restored_created = vsag::Factory::CreateIndex("hnsw", parameters(ip));
        check(restored_created.has_value(), "VSAG restore CreateIndex failed");
        auto restored = restored_created.value();
        check(restored->Deserialize(storage).has_value(), "VSAG Deserialize failed");
        verify_distances(restored, vectors, ids, ip);
        size_t hits = 0;
        for (size_t q = 0; q < nq; ++q) {
          const float *probe = probes.data() + q * dim;
          std::vector<std::pair<double, int64_t>> truth;
          for (size_t row = 0; row < n; ++row)
            truth.emplace_back(distance(probe, vectors.data() + row * dim, ip), ids[row]);
          std::sort(truth.begin(), truth.end());
          const auto answer = search(index, probe, vectors, ip);
          const auto reloaded = search(restored, probe, vectors, ip);
          check(answer.ids == reloaded.ids && answer.distances == reloaded.distances,
              "VSAG persistence changed ANN results");
          for (const auto id : answer.ids)
            hits += std::any_of(truth.begin(), truth.begin() + k,
                [&](const auto &entry) { return entry.second == id; });
        }
        retained.push_back(index);
        retained.push_back(restored);
        // Earlier indexes must survive the next bridge open/build/close cycle.
        for (size_t j = 0; j < retained.size(); ++j)
          search(retained[j], probes.data(), vectors, j >= 4);
        std::cout << "RESULT_JSON {\"metric\":\"" << metric_name << "\",\"repetition\":" << repetition
            << ",\"backend\":" << stats.backend << ",\"gpu_build_submissions\":" << stats.gpu_build_submissions
            << ",\"synchronized\":" << stats.synchronized << ",\"gpu_event_span_ms\":"
            << std::setprecision(10) << stats.gpu_build_ms << ",\"bridge_wall_ms\":" << stats.bridge_wall_ms
            << ",\"queries\":" << nq << ",\"k\":" << k << ",\"exact_hits\":" << hits
            << ",\"recall_at_10\":" << double(hits) / double(nq * k)
            << ",\"mapped_elements\":512,\"distance_comparisons\":524288,"
            << "\"persistence_identical\":true}" << std::endl;
        std::cout << "GPU_DEVICE " << stats.device_name << "\nGPU_VERSION " << stats.version << std::endl;
        if (repetition == 0 && std::getenv("HIPVS_SMOKE_SKIP_NEGATIVE") == nullptr) {
          rejects([&] { small.finalize(argv[1], config, 200, 4, 0, 1ULL << 24, stats); }, "no CPU fallback");
          check(stats.backend == 0 && stats.gpu_build_submissions == 0 && stats.synchronized == 0,
              "failure after success retained stale GPU statistics");
          verify_sentinel();
          snapshot(metric_name + "_failure_between_successful_builds");
        }
      }
    }
    retained.clear();
    verify_sentinel();
    snapshot("all_gpu_indexes_destroyed");
    std::cout << "PASS_ALL same_process_gpu_build_public_vsag_import repeated_bridge_lifecycle"
        << " explicit_gpu_failure live_vsag_integrity raw_l2_ip persistence; SQL_NOT_TESTED; NO_PERFORMANCE_CLAIM"
        << std::endl;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    snapshot("failure");
    return 1;
  }
}
