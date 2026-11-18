// Design-A feasibility probe: does a kernel-layout column file, mmap'd and scanned
// without any decode/transpose, beat the current typed cache and stay exact?
#include <immintrin.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <random>
#include <thread>
#include <vector>

namespace {
constexpr int N = 10000, D = 128, Q = 1000, K = 10;
constexpr std::size_t BLOCK = 2048;  // 16 rows x 128 dims
struct Cand { float d; int key; };
struct Better { bool operator()(const Cand& a, const Cand& b) const { return a.d < b.d || (a.d == b.d && a.key < b.key); } };

std::vector<int> reference(const std::vector<std::int16_t>& k, const std::int16_t* q) {
  std::vector<Cand> all(N);
  for (int i = 0; i < N; ++i) {
    std::int64_t s = 0;
    for (int d = 0; d < D; ++d) { const std::int64_t df = std::int64_t(k[std::size_t(i) * D + d]) - q[d]; s += df * df; }
    all[i] = Cand{float(std::sqrt(double(s) / 16384.0)), i};
  }
  std::sort(all.begin(), all.end(), [](const Cand& a, const Cand& b) { return a.d < b.d || (a.d == b.d && a.key < b.key); });
  std::vector<int> out; for (int i = 0; i < K; ++i) out.push_back(all[i].key); return out;
}

#if defined(__x86_64__) && defined(__GNUC__)
__attribute__((target("avx512f,avx512bw"), noinline))
void blk_i16(const std::int16_t* block, const std::int16_t* qk, std::int64_t* out) {
  __m512i acc32 = _mm512_setzero_si512(), lo = acc32, hi = acc32;
  for (int pair = 0; pair < D / 2; ++pair) {
    const __m512i q = _mm512_set1_epi32(int(std::uint16_t(qk[2 * pair])) | (int(qk[2 * pair + 1]) << 16));
    const __m512i v = _mm512_loadu_si512(reinterpret_cast<const void*>(block + pair * 32));
    const __m512i diff = _mm512_sub_epi16(q, v);
    acc32 = _mm512_add_epi32(acc32, _mm512_madd_epi16(diff, diff));
    if ((pair & 15) == 15) {
      lo = _mm512_add_epi64(lo, _mm512_cvtepi32_epi64(_mm512_castsi512_si256(acc32)));
      hi = _mm512_add_epi64(hi, _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(acc32, 1)));
      acc32 = _mm512_setzero_si512();
    }
  }
  lo = _mm512_add_epi64(lo, _mm512_cvtepi32_epi64(_mm512_castsi512_si256(acc32)));
  hi = _mm512_add_epi64(hi, _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(acc32, 1)));
  _mm512_storeu_si512(reinterpret_cast<void*>(out), lo);
  _mm512_storeu_si512(reinterpret_cast<void*>(out + 8), hi);
}
__attribute__((target("avx512f"), noinline))
void blk_f32(const float* block, const float* q, float* out) {
  __m512 acc = _mm512_setzero_ps();
  for (int d = 0; d < D; ++d) {
    const __m512 diff = _mm512_sub_ps(_mm512_set1_ps(q[d]), _mm512_loadu_ps(block + d * 16));
    acc = _mm512_fmadd_ps(diff, diff, acc);
  }
  _mm512_storeu_ps(out, acc);
}
#endif

void* map_file(const char* path, std::size_t bytes, bool create) {
  const int fd = ::open(path, create ? (O_RDWR | O_CREAT | O_TRUNC) : O_RDONLY, 0600);
  if (fd < 0) { std::perror("open"); std::exit(2); }
  if (create && ::ftruncate(fd, off_t(bytes)) != 0) { std::perror("ftruncate"); std::exit(2); }
  void* p = ::mmap(nullptr, bytes, create ? (PROT_READ | PROT_WRITE) : PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) { std::perror("mmap"); std::exit(2); }
  ::close(fd);
  return p;
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
  for (int i = 0; i < Q; ++i) truth[i] = reference(k, qs[i].data());

  // --- write kernel-layout columns (this is what a v3 format would persist) ---
  const std::size_t i16_bytes = std::size_t(N) * D * 2;      // 2.56 MB
  const std::size_t f32_bytes = std::size_t(N) * D * 4;      // 5.12 MB
  auto* col16 = static_cast<std::int16_t*>(map_file("/tmp/vectors.col16", i16_bytes, true));
  auto* col32 = static_cast<float*>(map_file("/tmp/vectors.col32", f32_bytes, true));
  for (int b = 0; b < N / 16; ++b)
    for (int d = 0; d < D; ++d)
      for (int lane = 0; lane < 16; ++lane) {
        const auto v = k[std::size_t(b * 16 + lane) * D + d];
        col16[std::size_t(b) * BLOCK + (d / 2) * 32 + lane * 2 + (d & 1)] = v;
        col32[std::size_t(b) * BLOCK + d * 16 + lane] = float(v) / 128.0f;
      }
  ::msync(col16, i16_bytes, MS_SYNC); ::msync(col32, f32_bytes, MS_SYNC);
  ::munmap(col16, i16_bytes); ::munmap(col32, f32_bytes);
  auto* r16 = static_cast<const std::int16_t*>(map_file("/tmp/vectors.col16", i16_bytes, false));
  auto* r32 = static_cast<const float*>(map_file("/tmp/vectors.col32", f32_bytes, false));

  auto run = [&](const char* name, bool use16, int threads) {
    auto t0 = std::chrono::steady_clock::now();
    double ratio = 0;
    std::vector<std::vector<int>> got(Q);
    auto worker = [&](int lo, int hi) {
      std::vector<std::int64_t> sums(16);
      std::array<float, 16> fsums{};
      for (int qi = lo; qi < hi; ++qi) {
        std::array<std::int16_t, D> qk{}; std::array<float, D> qf{};
        for (int d = 0; d < D; ++d) { qk[d] = qs[qi][d]; qf[d] = float(qs[qi][d]) / 128.0f; }
        std::priority_queue<Cand, std::vector<Cand>, Better> heap;
        for (int b = 0; b < N / 16; ++b) {
#if defined(__x86_64__) && defined(__GNUC__)
          if (use16) blk_i16(r16 + std::size_t(b) * BLOCK, qk.data(), sums.data());
          else blk_f32(r32 + std::size_t(b) * BLOCK, qf.data(), fsums.data());
#else
          (void)use16;
#endif
          for (int lane = 0; lane < 16; ++lane) {
            const double sq = use16 ? double(sums[lane]) / 16384.0 : double(fsums[lane]);
            Cand x{float(std::sqrt(sq)), b * 16 + lane};
            if (int(heap.size()) < K) heap.push(x);
            else if (Better{}(x, heap.top())) { heap.pop(); heap.push(x); }
          }
        }
        auto& out = got[qi];
        while (!heap.empty()) { out.push_back(heap.top().key); heap.pop(); }
        std::reverse(out.begin(), out.end());
      }
    };
    if (threads == 1) worker(0, Q);
    else {
      std::thread t([&] { worker(Q / 2, Q); });
      worker(0, Q / 2);
      t.join();
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    for (int qi = 0; qi < Q; ++qi) {
      int common = 0;
      for (int i = 0; i < K; ++i) if (got[qi][i] == truth[qi][i]) ++common;
      ratio += common / double(K);
    }
    std::printf("%-22s threads=%d  %8.2f ms   exact-top10=%.2f%%   scan=%s\n",
                name, threads, ms, 100 * ratio / Q, use16 ? "2.56 MB/q" : "5.12 MB/q");
    return ms;
  };
  run("int16 column, mmap", true, 1);
  run("float32 column, mmap", false, 1);
  run("int16 column, mmap", true, 2);
  run("float32 column, mmap", false, 2);
  ::munmap(const_cast<std::int16_t*>(r16), i16_bytes);
  ::munmap(const_cast<float*>(r32), f32_bytes);
  return 0;
}
