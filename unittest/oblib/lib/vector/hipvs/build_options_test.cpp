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
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace oceanbase::hipvs;
static void check(bool condition) { if (!condition) throw std::runtime_error("HIPVS build options contract failed"); }
template <typename F> static void rejects(F fn) {
  bool rejected = false;
  try { fn(); } catch (const std::invalid_argument &) { rejected = true; }
  check(rejected);
}
int main() {
  try {
    unsetenv("SEEKDB_HNSW_BUILD_BACKEND");
    check(selected_build_backend() == BuildBackend::CPU);
    setenv("SEEKDB_HNSW_BUILD_BACKEND", "cpu", 1);
    check(selected_build_backend() == BuildBackend::CPU);
    setenv("SEEKDB_HNSW_BUILD_BACKEND", "hipvs", 1);
    check(selected_build_backend() == BuildBackend::HIPVS);
    setenv("SEEKDB_HNSW_BUILD_BACKEND", "HIPVS", 1);
    rejects([] { selected_build_backend(); });
    setenv("SEEKDB_HNSW_BUILD_BACKEND", "", 1);
    rejects([] { selected_build_backend(); });
    unsetenv("SEEKDB_HIPVS_WORKER");
    unsetenv("SEEKDB_HIPVS_WORKER_TIMEOUT_SECONDS");
    unsetenv("SEEKDB_HIPVS_BRIDGE");
    unsetenv("SEEKDB_HIPVS_CPU_THREADS");
    rejects([] { BuildOptions::from_environment(); });
    setenv("SEEKDB_HIPVS_BRIDGE", "/tmp/test-bridge.so", 1);
    rejects([] { BuildOptions::from_environment(); });
    setenv("SEEKDB_HIPVS_CPU_THREADS", "4", 1);
    auto options = BuildOptions::from_environment();
    check(options.input_limit == (1ULL << 30) && options.export_limit == (1ULL << 30));
    check(options.cpu_threads == 4 && options.device == 0 && options.library_path == "/tmp/test-bridge.so");
    check(options.worker_path.empty() && options.worker_timeout_seconds == 1800);
    setenv("SEEKDB_HIPVS_WORKER", "/tmp/wrapper", 1);
    setenv("SEEKDB_HIPVS_WORKER_TIMEOUT_SECONDS", "42", 1);
    options = BuildOptions::from_environment();
    check(options.worker_path == "/tmp/wrapper" && options.worker_timeout_seconds == 42);
    setenv("SEEKDB_HIPVS_WORKER", "relative", 1);
    rejects([] { BuildOptions::from_environment(); });
    setenv("SEEKDB_HIPVS_WORKER", "", 1);
    rejects([] { BuildOptions::from_environment(); });
    unsetenv("SEEKDB_HIPVS_WORKER");
    const char *positive_names[] = {"SEEKDB_HIPVS_MAX_INPUT_BYTES", "SEEKDB_HIPVS_MAX_EXPORT_BYTES", "SEEKDB_HIPVS_CPU_THREADS", "SEEKDB_HIPVS_WORKER_TIMEOUT_SECONDS"};
    for (const auto *name : positive_names) {
      for (const auto *invalid : {"0", "-1", "+1", " 1", "1x", "", "18446744073709551616"}) {
        setenv(name, invalid, 1);
        rejects([] { BuildOptions::from_environment(); });
      }
      unsetenv(name);
      setenv("SEEKDB_HIPVS_CPU_THREADS", "4", 1);
    }
    setenv("SEEKDB_HIPVS_WORKER_TIMEOUT_SECONDS", "2147483648", 1);
    rejects([] { BuildOptions::from_environment(); });
    unsetenv("SEEKDB_HIPVS_WORKER_TIMEOUT_SECONDS");
    setenv("SEEKDB_HIPVS_CPU_THREADS", "2147483648", 1);
    rejects([] { BuildOptions::from_environment(); });
    setenv("SEEKDB_HIPVS_CPU_THREADS", "4", 1);
    for (const auto *invalid : {"-1", "+1", "1x", "", "2147483648"}) {
      setenv("SEEKDB_HIPVS_DEVICE", invalid, 1);
      rejects([] { BuildOptions::from_environment(); });
    }
    setenv("SEEKDB_HIPVS_DEVICE", "2", 1);
    setenv("SEEKDB_HIPVS_MAX_INPUT_BYTES", "18446744073709551615", 1);
    setenv("SEEKDB_HIPVS_MAX_EXPORT_BYTES", "42", 1);
    options = BuildOptions::from_environment();
    check(options.device == 2 && options.input_limit == std::numeric_limits<uint64_t>::max() && options.export_limit == 42);
    setenv("SEEKDB_HIPVS_BRIDGE", "relative.so", 1);
    rejects([] { BuildOptions::from_environment(); });
    BuildBatch batch(2, 1024);
    float vectors[] = {1, 2};
    int64_t ids[] = {15};
    batch.append(vectors, ids, 2, 1);
    SeekdbHipvsBuildStats stats{}; stats.backend = SEEKDB_HIPVS_AMD_BACKEND;
    bool failed = false;
    try { batch.finalize("/no/such/bridge.so", {2, 4, "l2", "{}"}, 100, 4, 0, 1024, stats); }
    catch (const std::exception &) { failed = true; }
    check(stats.backend == 0);
    check(failed && batch.count() == 1 && batch.vectors()[0] == 1 && batch.ids()[0] == 15);
    std::cout << "PASS: backend selection, checked environment options and failed-finalize ownership\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
