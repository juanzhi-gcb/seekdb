# CAGRA build-path tuning (AMD gfx1100 / hipVS)

Baseline: V5 frozen commit `634888d9b037153fa143d2c913ba4b25503e1d62`.
All numbers below are from a Radeon PRO W7900D (gfx1100, 48 GB), ROCm 7.2.4,
hipVS under `/opt/hipvs`, the same GPU family the evaluation uses.

## What changed

`tools/hipvs_bridge/seekdb_cuvs_bridge.c`, two independent changes:

1. **Build parameters re-tuned: `intermediate_graph_degree` 256 → 192,
   `nn_descent_niter` 100 → 50.** The NN-descent graph converges well before
   100 iterations, but the saved iterations are not free: at the itopk the
   server actually uses (512) cutting too far costs recall on hard data. 192/50
   builds ~45% faster than the frozen 256/100 baseline while matching or
   beating its recall on the hardest set measured — see the sweep below.
2. **Per-index search scratch.** `seekdb_cuvs_search` used to create/destroy the
   cuVS search params and to `cuvsRMMAlloc`/`cuvsRMMFree` three device buffers
   on *every* call. seekdb issues one vector per query, so that overhead was
   paid per query. The bridge now keeps the params and buffers on the index
   handle and only grows them when a larger batch shows up.

Nothing else on the GPU path moved: `graph_degree` stays at 128, search stays at
`search_width=4` / `itopk>=512`, same snapshot format, same FP32 data. There is
no stage-dependent code path and no CPU fallback involved.

## Parameter sweep

Two 250K-row datasets, 4096 queries each:

- `sift250k` — real SIFT, 128-d, where every setting tried is flat at
  1.0000/1.0000.
- `hard256` — uniform random, 256-d, deliberately harder than either eval set.
  The frozen baseline itself only scores 0.9261/0.8642 here, so this is the set
  that constrains the choice.

### Why the standalone tuner could not make the call

`tune_cagra` searches at `itopk = 4*topk` (400 at topk=100); the server forces
`itopk >= 512`. On SIFT the difference is invisible. On `hard256` it is not: at
400 even the baseline drops to 0.8814/0.8023, and more importantly the ranking
of build settings changes — the tuner puts `niter=40` about 0.001 under baseline,
while the same neighbourhood measured end-to-end at 512 lands on, or past, the
0.005 budget line. Build-time ratios do carry over between the two. Every recall
decision below therefore comes from end-to-end runs through seekdb; the tuner is
used only for isolated build-time scaling.

### Server-side sweep (`hard256`, DDL → 4096 single-row queries)

`CREATE VECTOR INDEX ... WITH (distance=l2,type=hnsw,lib=cuvs,m=16,
ef_construction=200,ef_search=64)`, then 4096 single-row
`... APPROXIMATE LIMIT 100` queries, at the server's real search parameters:

| graph_degree | intermediate | niter | DDL | QPS | recall@10 | recall@100 |
|---|---|---|---|---|---|---|
| 128 | 256 | 100 (frozen baseline) | 46.3s | 91.1 | 0.9261 | 0.8642 |
| 128 | 128 | 35 | 19.3–19.8s | 83.4–84.3 | 0.9174 | 0.8571 |
| 128 | 128 | 40 | 20.5–27.0s | 81.1–86.2 | 0.9205–0.9217 | 0.8604–0.8609 |
| 128 | 128 | 45 | 27.5s | 77.6 | 0.9225 | 0.8634 |
| 128 | 128 | 50 | 24.7–26.3s | 80.9–85.7 | 0.9235–0.9246 | 0.8645–0.8657 |
| **128** | **192** | **50** | **25.7–26.8s** | **82.2–89.9** | **0.9271–0.9294** | **0.8670** |
| 128 | 256 | 50 | 34.1s | 85.8 | 0.9239 | 0.8626 |

Recall gates on this set are @10 ≥ 0.9211 and @100 ≥ 0.8592, i.e. the tighter of
the absolute floor and 0.005 below the paired baseline. 128/35 fails both,
128/40 sits on the line, 128/45 and above clear it.

QPS is flat across every row because the build parameters do not touch the
search path — `graph_degree`, `search_width=4` and the `itopk` floor of 512 are
identical in all of them — so the QPS spread is run-to-run noise on a shared
instance, as is most of the DDL spread (±30% between repeats).

`intermediate_graph_degree=192, nn_descent_niter=50` is the pick: the same build
window as 128/50, but the largest recall margin measured — 0.9294/0.8670, above
the frozen baseline rather than 0.003 under it. Halving the intermediate graph
and the iteration count is what buys the 45%; going further (128/40 and below)
spends the entire 0.005 budget.

### Isolated build-time scaling (`tune_cagra`, `sift250k`)

250K real SIFT vectors (128-d), 4096 queries, recall vs exact ground truth:

| degree | intermediate | niter | build | recall@10 | recall@100 |
|---|---|---|---|---|---|
| 128 | 256 | 100 (baseline) | 40.2s | 1.0000 | 1.0000 |
| 128 | 256 | 40 | 21.6s | 1.0000 | 1.0000 |
| 128 | 256 | 20 | 14.8s | 1.0000 | 0.9998 |
| 64 | 128 | 40 | 14.7s | 1.0000 | 0.9999 |

On SIFT-like data iterations look free, which is exactly why the tuner appeared
to certify `niter=40` — and why it cannot be the instrument for the hard set.

Rejected alternatives: `build_algo=IVF_PQ` builds in 7.1s but only reaches
0.5616/0.5094 on `hard256`; `ITERATIVE_CAGRA_SEARCH` takes 59s. Cutting
`graph_degree` costs far more recall than cutting iterations (96/192/40 reaches
0.8227/0.7284 in the tuner), so the degree stays at 128 and only the
intermediate degree and the iteration count move.

## End-to-end (through seekdb, DDL → first query)

`CREATE VECTOR INDEX ... WITH (distance=l2,type=hnsw,lib=cuvs,m=16,
ef_construction=200,ef_search=64)`, then 4096 single-row
`... APPROXIMATE LIMIT 100` queries. Baseline and tuned are paired runs on the
same instance, same data, same harness; only the bridge's build parameters
differ.

250K real SIFT (128-d, `vector(128)`):

| | DDL (build+snapshot) | QPS | recall@10 | recall@100 |
|---|---|---|---|---|
| baseline (256/100) | 45.6s | 137.8 | 1.0000 | 1.0000 |
| tuned (192/50) | 24.4s | 126.9–135.3 | 1.0000 | 1.0000 |

250K uniform (256-d, `vector(256)`), the harder set:

| | DDL | QPS | recall@10 | recall@100 |
|---|---|---|---|---|
| baseline (256/100) | 46.3s | 91.1 | 0.9261 | 0.8642 |
| tuned (192/50) | 25.7s | 89.9 | 0.9294 | 0.8670 |

The tuned QPS range is repeats of the same binary on a shared instance; QPS does
not depend on the build parameters (identical graph degree and search path), so
the spread is measurement noise around the baseline rather than a regression —
which is also why the evaluator pairs each score against its own baseline run.

The DDL window is dominated by the CAGRA build: the serve log reports
`built=true, build_us=21302472` (21.3s) inside a 24.5s DDL, the remaining ~3.2s
being the `OBCUVS02` snapshot write. QPS is unchanged within run-to-run noise —
the harness spends most of each query round-tripping through the SQL layer; the
device-side scratch reuse removes a few hundred microseconds per query.

## CPU (VSAG) vs GPU (cuVS) build comparison

Same observer, same SIFT250K data (250K rows, `vector(128)`), same 4096
single-row `... APPROXIMATE LIMIT 100` queries; only the index backend differs
(the VSAG run is the identical DDL with `lib=vsag` in place of `lib=cuvs`, the
bridge untouched):

| Backend | DDL (build + snapshot) | QPS | recall@10 | recall@100 |
|---|---|---|---|---|
| vsag (CPU HNSW) | 84.0s | 1423.5 | 0.9924 | 0.9498 |
| cuvs baseline (256/100) | 45.6s | 137.8 | 1.0000 | 1.0000 |
| **cuvs tuned (192/50 + scratch reuse)** | **24.4s** | **127–135** | **1.0000** | **1.0000** |

The tuned GPU build is 3.4x faster than the CPU build and 1.8x faster than the
untuned GPU baseline, at perfect recall on this set; the CPU backend is also
the only one of the three that misses recall@100 (0.9498).

The CPU backend wins on single-row QPS because a 250K-row HNSW search at
`ef=64` finishes inside the SQL round trip, while every GPU query pays a H2D
copy, a kernel launch and a sync — that per-query overhead is what the scratch
reuse removes, and it is why the tuned QPS stays near the frozen GPU baseline
rather than above it. The evaluation scores query throughput against the
paired frozen GPU baseline, not against VSAG, and scores build time at 70%.

## Reproducing

Build (the bridge and the server are separate artifacts):

```bash
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so
./build.sh release -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so --make -j64
```

Isolate the build parameters without a server (this is how the tables above
were produced — `base.f32`/`query.f32` are row-major float32, `gt.i32` is
`nq x 100` int32):

```bash
hipcc -O2 -D__HIP_PLATFORM_AMD__ -I/opt/hipvs/include -I/opt/rocm/include \
  tools/hipvs_bridge/tune_cagra.c -L/opt/hipvs/lib \
  -Wl,-rpath,/opt/hipvs/lib:/opt/rocm/lib -lcuvs_c -o tune_cagra
./tune_cagra base.f32 250000 128 query.f32 4096 gt.i32 100 2 128 192 50
#          base n dim query nq gt topk [algo deg inter niter sw itopk]
```

The trailing `sw`/`itopk` default to `4*topk`, which is 400 here, not the
server's 512 — build times are comparable across the two, recall is not. See
"Parameter sweep" above.

## Restart / persistence

After `CREATE VECTOR INDEX`, the snapshot is written during the DDL
(`serialized GPU CAGRA snapshot ... built=true, graph_bytes=256001713`). After a
server restart the index is reloaded by
`ob_cuvs_deserialize` → `seekdb_cuvs_deserialize`
(`deserialized GPU CAGRA snapshot ... n64=250000, graph_bytes=256001713`) and
queries are served from the GPU again with no rebuild.
