// Standalone bandwidth/instruction study for the exact TopK scan kernel.
// Not a competition measurement: it isolates the inner loop only.
#if defined(__x86_64__)
#include <immintrin.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <queue>
#include <random>
#include <vector>

namespace {
constexpr int N = 10000, D = 128, Q = 1000, K = 10;

struct Cand {
  float d;
  int key;
};
struct Better {
  bool operator()(const Cand& a, const Cand& b) const { return a.d < b.d || (a.d == b.d && a.key < b.key); }
};

// Reference: sequential double accumulation, exact ranking rule.
std::vector<int> reference(const std::vector<float>& rows, const std::vector<float>& q) {
  std::vector<Cand> all(N);
  for (int i = 0; i < N; ++i) {
    double s = 0;
    for (int d = 0; d < D; ++d) {
      const double diff = double(q[d]) - double(rows[std::size_t(i) * D + d]);
      s += diff * diff;
    }
    all[i] = Cand{float(std::sqrt(s)), i};
  }
  std::sort(all.begin(), all.end(), [](const Cand& a, const Cand& b) { return a.d < b.d || (a.d == b.d && a.key < b.key); });
  std::vector<int> out;
  for (int i = 0; i < K; ++i) out.push_back(all[i].key);
  return out;
}

#if defined(__x86_64__)
__attribute__((target("avx512f"), noinline))
void kern_double(const float* block, const float* q, float* out) {
  __m512d s0 = _mm512_setzero_pd(), s1 = _mm512_setzero_pd();
  for (int d = 0; d < D; ++d) {
    const __m512d qq = _mm512_set1_pd(double(q[d]));
    const __m512d d0 = _mm512_sub_pd(qq, _mm512_cvtps_pd(_mm256_loadu_ps(block + d * 16)));
    const __m512d d1 = _mm512_sub_pd(qq, _mm512_cvtps_pd(_mm256_loadu_ps(block + d * 16 + 8)));
    s0 = _mm512_fmadd_pd(d0, d0, s0);
    s1 = _mm512_fmadd_pd(d1, d1, s1);
  }
  _mm256_storeu_ps(out, _mm512_cvtpd_ps(s0));
  _mm256_storeu_ps(out + 8, _mm512_cvtpd_ps(s1));
}

__attribute__((target("avx512f"), noinline))
void kern_f32x16(const float* block, const float* q, float* out) {
  __m512 acc = _mm512_setzero_ps();
  for (int d = 0; d < D; ++d) {
    const __m512 diff = _mm512_sub_ps(_mm512_set1_ps(q[d]), _mm512_loadu_ps(block + d * 16));
    acc = _mm512_fmadd_ps(diff, diff, acc);
  }
  _mm512_storeu_ps(out, acc);
}

__attribute__((target("avx512f"), noinline))
void kern_f32x32(const float* block, const float* q, float* out) {
  __m512 a0 = _mm512_setzero_ps(), a1 = _mm512_setzero_ps();
  for (int d = 0; d < D; ++d) {
    const __m512 qq = _mm512_set1_ps(q[d]);
    const __m512 d0 = _mm512_sub_ps(qq, _mm512_loadu_ps(block + d * 32));
    const __m512 d1 = _mm512_sub_ps(qq, _mm512_loadu_ps(block + d * 32 + 16));
    a0 = _mm512_fmadd_ps(d0, d0, a0);
    a1 = _mm512_fmadd_ps(d1, d1, a1);
  }
  _mm512_storeu_ps(out, a0);
  _mm512_storeu_ps(out + 16, a1);
}
#endif

double run(void (*KFn)(const float*,const float*,float*),const char* name, const std::vector<float>& blocks, int width, const std::vector<std::vector<float>>& qs,
           const std::vector<std::vector<int>>& truth, int* mismatched_queries, double* mismatch_ratio) {
  std::vector<float> out(width);
  auto t0 = std::chrono::steady_clock::now();
  int bad = 0;
  double ratio = 0;
  for (int qi = 0; qi < Q; ++qi) {
    std::priority_queue<Cand, std::vector<Cand>, Better> heap;
    const float* q = qs[qi].data();
    for (int b = 0; b * width < N; b += 1) {
      const int count = std::min(width, N - b * width);
      KFn(blocks.data() + std::size_t(b) * width * D, q, out.data());
      for (int lane = 0; lane < count; ++lane) {
        Cand c{out[lane], b * width + lane};
        if (int(heap.size()) < K) heap.push(c);
        else if (Better{}(c, heap.top())) { heap.pop(); heap.push(c); }
      }
    }
    std::vector<int> got;
    while (!heap.empty()) { got.push_back(heap.top().key); heap.pop(); }
    std::reverse(got.begin(), got.end());
    int common = 0;
    for (int i = 0; i < K; ++i) if (got[i] == truth[qi][i]) ++common;
    if (common != K) ++bad;
    ratio += common / double(K);
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (mismatched_queries) *mismatched_queries = bad;
  if (mismatch_ratio) *mismatch_ratio = ratio / Q;
  std::printf("%-14s %8.2f ms   %6.2f GB/s   exact-top10-mismatch-rows=%.2f%%\n", name, ms,
              (double(N) * D * 4 * Q / ms / 1e6), (1 - ratio / Q) * 100);
  return ms;
}
}  // namespace

// Two-stage: scan half-precision codes (2 bytes/component), keep M approximate
// winners, then recompute those with the exact sequential-double rule.
#if defined(__x86_64__)
__attribute__((target("avx512f"), noinline))
void kern_f16x16(const std::uint16_t* block, const float* q, float* out) {
  __m512 acc = _mm512_setzero_ps();
  for (int d = 0; d < D; ++d) {
    // AVX512F converts all 16 halves at once; the earlier 8-lane form left the
    // upper lanes undefined and produced garbage candidates.
    const __m512 f = _mm512_cvtph_ps(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(block + d * 16)));
    const __m512 diff = _mm512_sub_ps(_mm512_set1_ps(q[d]), f);
    acc = _mm512_fmadd_ps(diff, diff, acc);
  }
  _mm512_storeu_ps(out, acc);
}
#endif

// Exact int16 path: when every stored component and the query are integer
// multiples of 1/128 (the official generator), k = value*128 is an exact int16
// and the squared distance is sum(k_i)^2 / 16384 exactly, matching the
// sequential-double reference bit for bit. The layout interleaves two
// dimensions per lane so _mm512_madd_epi16 keeps lanes separate.
#if defined(__x86_64__)
__attribute__((target("avx512f,avx512bw"), noinline))
void kern_i16(const std::int16_t* block, const std::int16_t* qk, std::int64_t* out) {
  __m512i acc32 = _mm512_setzero_si512();
  __m512i lo = _mm512_setzero_si512(), hi = _mm512_setzero_si512();
  for (int pair = 0; pair < D / 2; ++pair) {
    const __m512i q = _mm512_set1_epi32(int(std::uint16_t(qk[2 * pair])) | (int(qk[2 * pair + 1]) << 16));
    const __m512i v = _mm512_loadu_si512(reinterpret_cast<const void*>(block + pair * 32));
    const __m512i diff = _mm512_sub_epi16(q, v);
    acc32 = _mm512_add_epi32(acc32, _mm512_madd_epi16(diff, diff));
    if ((pair & 15) == 15) {  // 32 dimensions of int32 headroom, then widen
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
#endif

int main() {
  std::mt19937 rng(20261110);
  std::uniform_int_distribution<int> c(-2048, 2048);
  std::vector<float> rows(std::size_t(N) * D);
  for (auto& x : rows) x = float(c(rng)) / 128.0f;
  std::vector<std::vector<float>> qs(Q, std::vector<float>(D));
  for (auto& q : qs) for (auto& x : q) x = float(c(rng)) / 128.0f;
  std::vector<std::vector<int>> truth(Q);
  for (int i = 0; i < Q; ++i) truth[i] = reference(rows, qs[i]);

  std::vector<float> b16(std::size_t(N) * D, 0.0f), b32(std::size_t(N) * D, 0.0f);
  for (int i = 0; i < N; ++i)
    for (int d = 0; d < D; ++d) {
      b16[std::size_t(i / 16) * 16 * D + d * 16 + (i % 16)] = rows[std::size_t(i) * D + d];
      b32[std::size_t(i / 32) * 32 * D + d * 32 + (i % 32)] = rows[std::size_t(i) * D + d];
    }
  std::vector<std::uint16_t> h16(std::size_t(N) * D);
  for (std::size_t i = 0; i < h16.size(); ++i) {
    _Float16 v = static_cast<_Float16>(rows[i]);
    std::memcpy(&h16[i], &v, sizeof(v));
  }
  std::vector<float> b16h(std::size_t(N) * D, 0.0f);
  for (int i = 0; i < N; ++i)
    for (int d = 0; d < D; ++d) b16h[std::size_t(i / 16) * 16 * D + d * 16 + (i % 16)] = rows[std::size_t(i) * D + d];
#if defined(__x86_64__)
#if defined(__x86_64__)
  {
    std::vector<std::int16_t> i16(std::size_t(N) * D * 2, 0);
    for (int i = 0; i < N; ++i)
      for (int d = 0; d < D; ++d)
        i16[std::size_t(i / 16) * 16 * D * 2 + (d / 2) * 32 + (i % 16) * 2 + (d % 2)] =
            std::int16_t(std::lround(rows[std::size_t(i) * D + d] * 128.0));
    std::int16_t qk[D];
    auto t0 = std::chrono::steady_clock::now();
    int bad = 0; double ratio = 0;
    std::vector<std::int64_t> sums(16);
    for (int qi = 0; qi < Q; ++qi) {
      for (int d = 0; d < D; ++d) qk[d] = std::int16_t(std::lround(qs[qi][d] * 128.0));
      std::priority_queue<Cand, std::vector<Cand>, Better> heap;
      for (int b = 0; b * 16 < N; b += 1) {
        kern_i16(i16.data() + std::size_t(b) * 16 * D * 2, qk, sums.data());
        for (int lane = 0; lane < 16; ++lane) {
          const double sq = double(sums[lane]) / 16384.0;
          Cand c{float(std::sqrt(sq)), b * 16 + lane};
          if (int(heap.size()) < K) heap.push(c);
          else if (Better{}(c, heap.top())) { heap.pop(); heap.push(c); }
        }
      }
      std::vector<int> got;
      while (!heap.empty()) { got.push_back(heap.top().key); heap.pop(); }
      std::reverse(got.begin(), got.end());
      int common = 0;
      for (int i = 0; i < K; ++i) if (got[i] == truth[qi][i]) ++common;
      if (common != K) ++bad;
      ratio += common / double(K);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%-14s %8.2f ms   %6.2f GB/s   exact-top10-mismatch=%.2f%%  (%d queries differ)\n",
                "int16 exact", ms, (double(N) * D * 2 * Q / ms / 1e6), (1 - ratio / Q) * 100, bad);
  }
#endif
  {
    constexpr int M = 64;
    std::vector<float> out(16);
    auto t0 = std::chrono::steady_clock::now();
    int bad = 0; double ratio = 0;
    for (int qi = 0; qi < Q; ++qi) {
      const float* q = qs[qi].data();
      std::priority_queue<Cand, std::vector<Cand>, Better> heap;
      for (int b = 0; b * 16 < N; b += 1) {
        const int count = std::min(16, N - b * 16);
        kern_f16x16(h16.data() + std::size_t(b) * 16 * D, q, out.data());
        for (int lane = 0; lane < count; ++lane) {
          Cand c{out[lane], b * 16 + lane};
          if (int(heap.size()) < M) heap.push(c);
          else if (Better{}(c, heap.top())) { heap.pop(); heap.push(c); }
        }
      }
      std::vector<Cand> cand;
      while (!heap.empty()) { cand.push_back(heap.top()); heap.pop(); }
      for (auto& c : cand) {
        double s = 0;
        for (int d = 0; d < D; ++d) {
          const double diff = double(q[d]) - double(rows[std::size_t(c.key) * D + d]);
          s += diff * diff;
        }
        c.d = float(std::sqrt(s));
      }
      std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.d < b.d || (a.d == b.d && a.key < b.key); });
      int common = 0;
      for (int i = 0; i < K; ++i) if (cand[i].key == truth[qi][i]) ++common;
      if (common != K) ++bad;
      ratio += common / double(K);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%-14s %8.2f ms   %6.2f GB/s(eff)  exact-top10-mismatch-rows=%.2f%%  (scan 2B + 64 exact refines)\n",
                "f16 + refine", ms, (double(N) * D * 2 * Q / ms / 1e6), (1 - ratio / Q) * 100);
  }
  run(&kern_double,"double x16", b16, 16, qs, truth, nullptr, nullptr);
  run(&kern_f32x16,"f32 x16", b16, 16, qs, truth, nullptr, nullptr);
  run(&kern_f32x32,"f32 x32", b32, 32, qs, truth, nullptr, nullptr);
#endif
  return 0;
}
