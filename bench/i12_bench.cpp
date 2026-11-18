// Can a cache-free scan of 12-bit packed rows beat the transposed float32 cache?
// Isolates decode+accumulate only; same 10000x128 workload, 1000 TopK queries.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <queue>
#include <random>
#include <vector>

namespace {
constexpr int N = 10000, D = 128, Q = 1000, K = 10;
struct Cand { float d; int key; };
struct Better { bool operator()(const Cand& a, const Cand& b) const { return a.d < b.d || (a.d == b.d && a.key < b.key); } };

std::vector<int> reference(const std::vector<std::int16_t>& k, const std::vector<std::int16_t>& q) {
  std::vector<Cand> all(N);
  for (int i = 0; i < N; ++i) {
    std::int64_t s = 0;
    for (int d = 0; d < D; ++d) { const std::int64_t diff = std::int64_t(k[std::size_t(i) * D + d]) - q[d]; s += diff * diff; }
    all[i] = Cand{float(std::sqrt(double(s) / 16384.0)), i};
  }
  std::sort(all.begin(), all.end(), [](const Cand& a, const Cand& b) { return a.d < b.d || (a.d == b.d && a.key < b.key); });
  std::vector<int> out; for (int i = 0; i < K; ++i) out.push_back(all[i].key); return out;
}
}  // namespace

int main() {
  std::mt19937 rng(20261110);
  std::uniform_int_distribution<int> c(-2048, 2047);
  std::vector<std::int16_t> k(std::size_t(N) * D);
  for (auto& x : k) x = std::int16_t(c(rng));
  std::vector<std::array<std::int16_t, D>> qs(Q);
  for (auto& q : qs) for (auto& x : q) x = std::int16_t(c(rng));
  std::vector<std::vector<int>> truth(Q);
  for (int i = 0; i < Q; ++i) truth[i] = reference(k, std::vector<std::int16_t>(qs[i].begin(), qs[i].end()));

  // Packed rows: 12 bits each, bias 2048.
  const std::size_t row_bytes = (D * 12 + 7) / 8;
  std::vector<unsigned char> packed(std::size_t(N) * row_bytes);
  for (int i = 0; i < N; ++i) {
    std::uint32_t acc = 0; int bits = 0; std::size_t at = std::size_t(i) * row_bytes;
    for (int d = 0; d < D; ++d) {
      acc |= std::uint32_t(std::uint16_t(k[std::size_t(i) * D + d] + 2048)) << bits; bits += 12;
      while (bits >= 8) { packed[at++] = static_cast<unsigned char>(acc & 0xff); acc >>= 8; bits -= 8; }
    }
  }

  // Variant A: transposed float32 blocks (current design), 16 lanes.
  std::vector<float> tf(std::size_t(N) * D);
  for (int i = 0; i < N; ++i)
    for (int d = 0; d < D; ++d) tf[std::size_t(i / 16) * 16 * D + d * 16 + (i % 16)] = float(k[std::size_t(i) * D + d]) / 128.0f;
  auto run_cache = [&](const char* name) {
    auto t0 = std::chrono::steady_clock::now();
    double ratio = 0;
    for (int qi = 0; qi < Q; ++qi) {
      std::array<float, D> qf{}; for (int d = 0; d < D; ++d) qf[d] = float(qs[qi][d]) / 128.0f;
      std::priority_queue<Cand, std::vector<Cand>, Better> heap;
      for (int b = 0; b * 16 < N; ++b) {
        std::array<float, 16> sums{};
        const float* block = tf.data() + std::size_t(b) * 16 * D;
        for (int d = 0; d < D; ++d) { const float qq = qf[d]; for (int lane = 0; lane < 16; ++lane) { const float df = qq - block[d * 16 + lane]; sums[lane] += df * df; } }
        for (int lane = 0; lane < 16; ++lane) { Cand x{std::sqrt(sums[lane]), b * 16 + lane}; if (int(heap.size()) < K) heap.push(x); else if (Better{}(x, heap.top())) { heap.pop(); heap.push(x); } }
      }
      std::vector<int> got; while (!heap.empty()) { got.push_back(heap.top().key); heap.pop(); }
      std::reverse(got.begin(), got.end());
      int common = 0; for (int i = 0; i < K; ++i) if (got[i] == truth[qi][i]) ++common; ratio += common / double(K);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%-16s %8.2f ms   exact=%.2f%%   bytes/q=%.2f MB\n", name, ms, 100 * ratio / Q, double(N) * D * 4 / 1048576.0);
  };

  // Variant B: cache-free, decode 12-bit per row then accumulate.
  auto run_packed = [&](const char* name) {
    auto t0 = std::chrono::steady_clock::now();
    double ratio = 0;
    std::vector<std::int16_t> row(D);
    for (int qi = 0; qi < Q; ++qi) {
      const auto& q = qs[qi];
      std::priority_queue<Cand, std::vector<Cand>, Better> heap;
      for (int i = 0; i < N; ++i) {
        const unsigned char* src = packed.data() + std::size_t(i) * row_bytes;
        std::uint32_t acc = 0; int bits = 0; std::size_t at = 0;
        std::int64_t sum = 0;
        for (int d = 0; d < D; ++d) {
          while (bits < 12) { acc |= std::uint32_t(src[at++]) << bits; bits += 8; }
          const int code = int(acc & 0xfff); acc >>= 12; bits -= 12;
          const std::int64_t diff = std::int64_t(code - 2048) - q[d]; sum += diff * diff;
        }
        Cand x{float(std::sqrt(double(sum) / 16384.0)), i};
        if (int(heap.size()) < K) heap.push(x); else if (Better{}(x, heap.top())) { heap.pop(); heap.push(x); }
      }
      std::vector<int> got; while (!heap.empty()) { got.push_back(heap.top().key); heap.pop(); }
      std::reverse(got.begin(), got.end());
      int common = 0; for (int i = 0; i < K; ++i) if (got[i] == truth[qi][i]) ++common; ratio += common / double(K);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%-16s %8.2f ms   exact=%.2f%%   bytes/q=%.2f MB\n", name, ms, 100 * ratio / Q, double(N) * row_bytes / 1048576.0);
  };

  run_cache("cache f32 x16");
  run_packed("packed12 stream");
  return 0;
}
