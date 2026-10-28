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
#include <iostream>
#include <stdexcept>
#include <limits>

using oceanbase::hipvs::BuildBatch;
void check(bool condition) { if (!condition) throw std::runtime_error("batch contract failed"); }
template <typename F> void rejects(F fn) {
  bool rejected = false;
  try { fn(); } catch (const std::exception &) { rejected = true; }
  check(rejected);
}

int main()
{
  try {
    BuildBatch batch(2, 1024);
    float values[] = {1, 2, 3, 4};
    int64_t ids[] = {101, 409};
    batch.append(values, ids, 2, 2);
    values[0] = 999;
    ids[0] = 999;
    check(batch.count() == 2 && batch.vectors()[0] == 1 && batch.ids()[0] == 101);
    rejects([&] { batch.append(values, ids, 3, 1); });
    float invalid[] = {std::numeric_limits<float>::infinity(), 1};
    rejects([&] { batch.append(invalid, ids, 2, 1); });
    check(batch.count() == 2);
    BuildBatch small(2, 16);
    small.append(values, ids, 2, 1);
    rejects([&] { small.append(values, ids, 2, 1); });
    rejects([&] { small.append(nullptr, ids, 2, 1); });
    int64_t negative = -1;
    rejects([&] { small.append(values, &negative, 2, 1); });
    // A missing optional GPU library must fail; no CPU fallback is allowed.
    rejects([&] { oceanbase::hipvs::GpuLibrary library("/no/such/hipvs-bridge.so"); });
    rejects([&] { oceanbase::hipvs::GpuLibrary library("relative.so"); });
    BuildBatch duplicates(2, 1024);
    const int64_t same_ids[] = {101, 101};
    duplicates.append(values, same_ids, 2, 2);
    rejects([&] { duplicates.validate_ids(); });
    std::cout << "PASS: buffered Add ownership, dimensions, finite data, limits, IDs and explicit GPU failure\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
