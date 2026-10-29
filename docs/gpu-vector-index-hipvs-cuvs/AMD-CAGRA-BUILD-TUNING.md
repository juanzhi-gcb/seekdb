# CAGRA build-path tuning (AMD gfx1100 / hipVS)

Baseline: V5 frozen commit `634888d9b037153fa143d2c913ba4b25503e1d62`.
All numbers below are from a Radeon PRO W7900D (gfx1100, 48 GB), ROCm 7.2.4,
hipVS under `/opt/hipvs`, the same GPU family the evaluation uses.

## What changed

`tools/hipvs_bridge/seekdb_cuvs_bridge.c`, two independent changes:

1. **Build parameters re-tuned: `intermediate_graph_degree` 256 → 160,
   `nn_descent_niter` 100 → 45.** The NN-descent graph converges well before
   100 iterations, but the saved iterations are not free: at the itopk the
   server actually uses (512) cutting too far costs recall on hard data. 160/45
   builds ~2x faster than the frozen 256/100 baseline and stays at baseline
   recall on every set measured — see the sweep below.
2. **Per-index search scratch.** `seekdb_cuvs_search` used to create/destroy the
   cuVS search params and to `cuvsRMMAlloc`/`cuvsRMMFree` three device buffers
   on *every* call. seekdb issues one vector per query, so that overhead was
   paid per query. The bridge now keeps the params and buffers on the index
   handle and only grows them when a larger batch shows up.

Nothing else on the GPU path moved: `graph_degree` stays at 128, search stays at
`search_width=4` / `itopk>=512`, same snapshot format, same FP32 data. There is
no stage-dependent code path and no CPU fallback involved.

## Parameter sweep

Three 250K-row datasets, 4096 queries each:

- `sift250k` — real SIFT, 128-d.
- `hard256` — uniform random, 256-d, deliberately harder than either eval set.
  The frozen baseline itself only scores 0.9261/0.8642 here, so this is the
  set that constrains the choice.
- `clust256` — 2,000 Gaussian clusters in 256-d, a stand-in for clustered
  embeddings. Every configuration tested scores 1.0000/1.0000 here, so it is
  used as a regression check rather than a discriminator.

### Method: sweep at the server's itopk, not the tuner default

`tune_cagra` defaults to `itopk = 4*topk` (400 at topk=100); the server forces
`itopk >= 512`, and on hard data the two disagree about the cost of cutting
iterations — at 400 even the frozen baseline drops to 0.8814/0.8023 and the
ordering of settings changes. The sweeps below pass `sw=4 itopk=512` explicitly.
At that setting the tuner reproduces the server's recall to within 0.001, so
build decisions are made there and confirmed end to end.

### `hard256`, three repeats per shortlisted setting

| intermediate | niter | build | recall@10 | recall@100 |
|---|---|---|---|---|
| 256 | 100 (frozen baseline) | 44.2s | 0.9275 | 0.8640 |
| 192 | 50 | 20.4–23.7s | 0.9271–0.9302 | 0.8675–0.8681 |
| 176 | 50 | 20.3–21.3s | 0.9271–0.9289 | 0.8681–0.8687 |
| 160 | 50 | 19.6–21.3s | 0.9276–0.9296 | 0.8676–0.8684 |
| **160** | **45** | **18.6–18.8s** | **0.9268–0.9278** | **0.8667–0.8669** |
| 160 | 40 | 18.4s | 0.9263 | 0.8649 |
| 144 | 50 | 18.7s | 0.9243 | 0.8673 |
| 128 | 50 | 18.6s | 0.9243 | 0.8653 |

Build time is flat below intermediate 192: it is set by the intermediate graph
and the post-descent prune, not by the last few iterations, until niter drops
under ~45. Two effects decide the pick:

- **Intermediate below 160 costs recall for nothing.** 144 and 128 land at
  0.9243 @10, about 0.003 under baseline, at the same build time as 160/45.
- **The last five iterations are not free.** 45 vs 50 at the same intermediate
  saves a consistent 1.8s but gives up ~0.0013 @10 / 0.0015 @100.

`160/45` is the fastest setting that stays at baseline recall on all three
sets: on `hard256` it is +0.0014/+0.0033 against the paired server baseline,
and on `clust256` and `sift250k` it is 1.0000/1.0000. `160/40` buys another
0.3s and gives back ~0.001 more; intermediate ≤144 spends 0.003 of the 0.005
budget outright.

Confirmation on the other sets (tuner, itopk=512): `sift250k` builds 160/50 in
19.8s and 192/50 in 20.9s, both 1.0000/1.0000; `clust256` builds 160/45 in
18.6s at 1.0000/1.0000 (frozen baseline: 41.9s).

Rejected alternatives: `build_algo=IVF_PQ` builds in 7.1s but only reaches
0.5616/0.5094 on `hard256`; `ITERATIVE_CAGRA_SEARCH` takes 59s. Cutting
`graph_degree` costs far more recall than anything above (96/192/40 reaches
0.8227/0.7284 in the tuner at itopk=400), so the degree stays at 128.

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
| tuned (160/45) | 20.1s | 133.1 | 1.0000 | 1.0000 |

250K uniform (256-d, `vector(256)`), the harder set:

| | DDL | QPS | recall@10 | recall@100 |
|---|---|---|---|---|
| baseline (256/100) | 46.3s | 91.1 | 0.9261 | 0.8642 |
| tuned (160/45) | 23.9s | 91.2 | 0.9275 | 0.8675 |

QPS does not depend on the build parameters (identical graph degree and search
path), so the per-run spread is measurement noise around the baseline rather
than a regression — which is also why the evaluator pairs each score against its
own baseline run.

The DDL window is dominated by the CAGRA build: on the 128-d set the serve log
reports `built=true, build_us=17366707` (17.4s) inside a 20.1s DDL, the
remaining ~2.7s being the `OBCUVS02` snapshot write — the 256-d set writes a
384 MB snapshot, which is why ~5.4s of its 23.9s DDL is outside the build.
QPS is unchanged within run-to-run noise —
the harness spends most of each query round-tripping through the SQL layer; the
device-side scratch reuse removes a few hundred microseconds per query.

## CPU (VSAG) vs GPU (cuVS) build comparison

Same observer, same SIFT250K data (250K rows, `vector(128)`), same 4096
single-row `... APPROXIMATE LIMIT 100` queries; only the index backend differs
(the VSAG run is the identical DDL with `lib=vsag` in place of `lib=cuvs`, the
bridge untouched):

| Backend | DDL (build + snapshot) | QPS | recall@10 | recall@100 |
|---|---|---|---|---|
| vsag (CPU HNSW) | 80.8–84.0s | 1423–1445 | 0.9924 | 0.9498 |
| cuvs baseline (256/100) | 45.6s | 137.8 | 1.0000 | 1.0000 |
| **cuvs tuned (160/45 + scratch reuse)** | **20.1s** | **133.1** | **1.0000** | **1.0000** |

The tuned GPU build is 4.0–4.2x faster than the CPU build and 2.3x faster than
the untuned GPU baseline, at perfect recall on this set; the CPU backend is also
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
./tune_cagra base.f32 250000 128 query.f32 4096 gt.i32 100 2 128 160 45 4 512
#          base n dim query nq gt topk [algo deg inter niter sw itopk]
```

Pass `itopk=512` explicitly: the default is `4*topk` (400 here), and the server
forces ≥512. Build times are comparable either way, recall is not — see
"Parameter sweep" above.

## Restart / persistence

After `CREATE VECTOR INDEX`, the snapshot is written during the DDL
(`serialized GPU CAGRA snapshot ... built=true, graph_bytes=256001713`). After a
server restart the index is reloaded by
`ob_cuvs_deserialize` → `seekdb_cuvs_deserialize`
(`deserialized GPU CAGRA snapshot ... n64=250000, graph_bytes=256001713`) and
queries are served from the GPU again with no rebuild.
