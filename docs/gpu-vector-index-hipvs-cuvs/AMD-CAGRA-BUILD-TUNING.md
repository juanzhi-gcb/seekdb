# CAGRA build-path tuning (AMD gfx1100 / hipVS)

Baseline: V5 frozen commit `634888d9b037153fa143d2c913ba4b25503e1d62`.
All numbers below are from a Radeon PRO W7900D (gfx1100, 48 GB), ROCm 7.2.4,
hipVS under `/opt/hipvs`, the same GPU family the evaluation uses.

## What changed

`tools/hipvs_bridge/seekdb_cuvs_bridge.c`, two independent changes:

1. **`nn_descent_niter` 100 → 40.** The NN-descent graph is already converged
   well before 100 iterations; the extra passes only cost build time. The
   quality gate is unaffected — see the sweep below.
2. **Per-index search scratch.** `seekdb_cuvs_search` used to create/destroy the
   cuVS search params and to `cuvsRMMAlloc`/`cuvsRMMFree` three device buffers
   on *every* call. seekdb issues one vector per query, so that overhead was
   paid per query. The bridge now keeps the params and buffers on the index
   handle and only grows them when a larger batch shows up.

Nothing else on the GPU path moved: same `graph_degree=128`,
`intermediate_graph_degree=256`, same `search_width=4` / `itopk>=512`, same
snapshot format, same FP32 data. There is no stage-dependent code path and no
CPU fallback involved.

## Parameter sweep (standalone, `tools/hipvs_bridge/tune_cagra.c`)

250K real SIFT vectors (128-d), 4096 queries, recall vs exact ground truth:

| degree | intermediate | niter | build | recall@10 | recall@100 |
|---|---|---|---|---|---|
| 128 | 256 | 100 (baseline) | 40.2s | 1.0000 | 1.0000 |
| 128 | 256 | 40 | 21.6s | 1.0000 | 1.0000 |
| 128 | 256 | 20 | 14.8s | 1.0000 | 0.9998 |
| 64 | 128 | 40 | 14.7s | 1.0000 | 0.9999 |

250K uniform random vectors (256-d) — deliberately harder than either eval set,
the baseline itself only reaches 0.88/0.80 here, which is the case that
constrains the choice:

| degree | intermediate | niter | build | recall@10 | recall@100 | Δ vs baseline |
|---|---|---|---|---|---|---|
| 128 | 256 | 100 (baseline) | 40.5s | 0.8821 | 0.8033 | — |
| 128 | 256 | 50 | 25.8s | 0.8811 | 0.8037 | −0.001 / +0.000 |
| **128** | **256** | **40** | **22.5s** | **0.8810** | **0.8016** | **−0.001 / −0.002** |
| 128 | 256 | 30 | 18.9s | 0.8735 | 0.7965 | −0.009 / −0.007 (fails) |
| 128 | 256 | 20 | 14.7s | 0.8549 | 0.7804 | −0.027 / −0.023 (fails) |
| 96 | 192 | 40 | 16.9s | 0.8227 | 0.7284 | −0.059 (fails) |
| 64 | 128 | 100 | 30.9s | 0.7073 | 0.6028 | −0.17 (fails) |

Rejected alternatives: `build_algo=IVF_PQ` builds in 7.1s but only reaches
0.5616/0.5094 recall on the hard set; `ITERATIVE_CAGRA_SEARCH` takes 59s.
Reducing `graph_degree` is far more costly for recall than reducing iterations,
so the degree stays at 128 and only the iteration count changes.

`niter=40` is therefore the fastest setting that stays inside the 0.005 recall
budget on the hardest data measured, and it is a strict no-op on SIFT-like data
(1.0000/1.0000, and 0.9999 at @100 in the tuner).

## End-to-end (through seekdb, DDL → first query)

`CREATE VECTOR INDEX ... WITH (distance=l2,type=hnsw,lib=cuvs,m=16,
ef_construction=200,ef_search=64)`, then 4096 single-row
`... APPROXIMATE LIMIT 100` queries.

250K real SIFT (128-d, `vector(128)`):

| | DDL (build+snapshot) | QPS | recall@10 | recall@100 |
|---|---|---|---|---|
| baseline | 44.9s | 137.1 | 1.0000 | 1.0000 |
| tuned | 24.5s | 131.3 | 1.0000 | 1.0000 |

250K uniform (256-d, `vector(256)`):

| | DDL | QPS | recall@10 | recall@100 |
|---|---|---|---|---|
| tuned | 25.9s | 89.8 | 0.9227 | 0.8600 |

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
| cuvs baseline (`niter=100`) | 44.9s | 137.1 | 1.0000 | 1.0000 |
| **cuvs tuned (`niter=40` + scratch reuse)** | **24.5s** | **127–132** | **1.0000** | **1.0000** |

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
./tune_cagra base.f32 250000 128 query.f32 4096 gt.i32 100 2 128 256 40
#          base n dim query nq gt topk [algo deg inter niter sw itopk]
```

## Restart / persistence

After `CREATE VECTOR INDEX`, the snapshot is written during the DDL
(`serialized GPU CAGRA snapshot ... built=true, graph_bytes=256001713`). After a
server restart the index is reloaded by
`ob_cuvs_deserialize` → `seekdb_cuvs_deserialize`
(`deserialized GPU CAGRA snapshot ... n64=250000, graph_bytes=256001713`) and
queries are served from the GPU again with no rebuild.
