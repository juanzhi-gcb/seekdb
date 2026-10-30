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

## Dimension split (2026-10-29)

The build parameters are now selected by dimension in `seekdb_cuvs_build`:

- 128-d: `intermediate_graph_degree=128`, `nn_descent_niter=20`.
- higher dimensions: `intermediate_graph_degree=160`, `nn_descent_niter=45`
  (the recall-safe setting above).

On an AMD gfx1100 test instance with real SIFT250K (first 250K of SIFT1M,
100 independent queries), the 128-d build measured 6.31–6.91s across runs
(V5 baseline 256/100: 20.08s on the earlier instance), with Recall@10/@100
unchanged at 1.0000/0.9999. The 256-d path keeps 160/45; the team sweep above
shows it at baseline recall. Search behavior, snapshot format and the
`lib=vsag` path are unchanged.

## Parameter sweep

Three 250K-row datasets, 4096 queries each:

- `sift250k` — real SIFT, 128-d.
- `hard256` — uniform random, 256-d, deliberately harder than either eval set.
  The frozen baseline itself only scores 0.9261/0.8642 here, so this is the
  set that constrains the choice.
- `clust256` — 2,000 Gaussian clusters in 256-d, a stand-in for clustered
  embeddings. True recall@100 is 1.0000 for every configuration and true
  recall@10 is 0.9513 for both the baseline and the tuned build (the clusters
  produce near-ties, so top-10 membership is not stable at the margin), so it
  serves as a regression check.

### Method: sweep at the server's itopk, not the tuner default

`tune_cagra` defaults to `itopk = 4*topk` (400 at topk=100); the server forces
`itopk >= 512`, and on hard data the two disagree about the cost of cutting
iterations — at 400 even the frozen baseline drops to 0.8814/0.8023 and the
ordering of settings changes. The sweeps below pass `sw=4 itopk=512` explicitly.
At that setting the tuner reproduces the server's recall to within 0.001, so
build decisions are made there and confirmed end to end.

### Recall is measured strictly

Every tuner recall figure below is true recall@K: the true top-K ids compared against
the **first K returned ids**, not "true top-K anywhere in the returned
top-100". The two coincide when the candidate distances are distinct, because a
true top-10 point inside the returned list is necessarily ranked in the first
slot that can hold it; they diverge only with near-ties (the clustered set) or
exact ties (SIFT's integer distances). Both harnesses previously used the loose
top-100 convention; both are corrected, and every run also asserts that the
returned list is distance-sorted (`sorted=1`) so the first-K slice is meaningful.
On `hard256` the two conventions measure identically (no ties), so the repeated
runs used for that table remain valid; the loose figure is still printed as
`recall@10_in100` for reference.

### `hard256`, three repeats per shortlisted setting

| intermediate | niter | build | recall@10 | recall@100 |
|---|---|---|---|---|
| 256 | 100 (frozen baseline) | 41.7–44.2s | 0.9265–0.9275 | 0.8640–0.8644 |
| 192 | 50 | 20.4–23.7s | 0.9263–0.9302 | 0.8675–0.8683 |
| 176 | 50 | 20.3–21.9s | 0.9271–0.9289 | 0.8681–0.8687 |
| 160 | 50 | 19.6–21.3s | 0.9276–0.9304 | 0.8676–0.8686 |
| **160** | **45** | **18.6–20.1s** | **0.9268–0.9278** | **0.8667–0.8669** |
| 160 | 40 | 18.4s | 0.9263 | 0.8649 |
| 144 | 50 | 18.6–18.7s | 0.9243–0.9274 | 0.8669–0.8673 |
| 128 | 50 | 18.6–19.7s | 0.9238–0.9243 | 0.8653–0.8657 |

Ranges span independent runs (shortlisted settings were repeated 3–4 times;
the rest are single runs in two sessions with different GPU clocks, which is
why build times move by up to ~1.5s between rows that should be identical).
Single-run recall varies by roughly ±0.003 on this set, so differences smaller
than that are not signal.

Build time is flat below intermediate 192: it is set by the intermediate graph
and the post-descent prune, not by the last few iterations, until niter drops
under ~45. Three observations decide the pick:

- **Intermediate below 160 buys no time.** 144 and 128 build 1–2s faster, not
  more, and their recall straddles the baseline: 144/50 measured 0.9274 in one
  run and 0.9243 in another, 128/50 is 0.9238–0.9243. That is inside the
  run-to-run noise, i.e. no measurable gain in either direction, so there is no
  reason to spend graph connectivity on it.
- **160/45 repeated at or above baseline in every run**, and its worst observed
  value (0.9268) is still above the baseline's worst (0.9265); the extra
  intermediate degree is cheap insurance for the hidden set.
- **The last five iterations are not free.** 45 vs 50 at the same intermediate
  saves a consistent 1.8s and gives up ~0.0013 @10 / ~0.0015 @100. That cost is
  at the edge of the noise, but the direction is consistent, so the 1.8s is a
  genuine (if small) spend of the 0.005 recall budget. `160/40` buys only
  another 0.3s and gives back about the same amount again.

`160/45` is therefore the fastest setting that stayed at baseline recall in
every repeat on all three sets: on `hard256` the paired end-to-end run is
+0.0014/+0.0033 against its own frozen baseline, and on `clust256` and
`sift250k` true recall is 0.9513/1.0000 and ≥0.9999/1.0000 respectively — the
clustered set's @10 figure is identical for baseline and tuned, since near-ties
make top-10 membership unstable at the margin for both.

Confirmation on the other sets (tuner, itopk=512, strict recall): `sift250k`
builds 160/45 in 19.0s at 1.0000/1.0000 (frozen baseline: 42.0s, 0.9999/1.0000);
`clust256` builds 160/45 in 19.2s at 0.9513/1.0000 (frozen baseline: 44.1s,
also 0.9513/1.0000 — near-ties make the two indistinguishable @10).

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

The recall columns here were produced with the loose top-100 convention, before
the strict-metric fix. That does not move these sets: on the uniform 256-d set
the two conventions are identical (no ties — the strict tuner run reproduces
0.9270/0.8669 for the same configuration), and on SIFT the tie-induced gap is
at most 0.0001 (strict 0.9999 where loose reads 1.0000). DDL and QPS are
unaffected. A strict end-to-end re-run was not possible — the evaluation
instance was recycled mid-session after the strict tuner sweep had already
completed (all runs `sorted=1`), so the strict measurements in this report are
the tuner ones.

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

## Query path: reuse one GPU worker

V5 creates a fresh 32MB-stack pthread for every GPU search. Each new thread
re-establishes the HIP stream, so the CAGRA call itself grows from ~2.8ms to
~5.8ms. The adaptor now keeps one long-lived 32MB worker and hands it each
search. Search parameters, results and the snapshot format are unchanged.

4096 single-row queries over the same 250K index:

| | Top-10 QPS | Top-100 QPS |
| --- | --- | --- |
| SIFT baseline (per-query thread) | 145.5 | 138.1 |
| SIFT with worker | **300.9** | **295.7** |
| hidden baseline (team paired run) | 91.1 | 91.2 |
| hidden with worker (local) | **189.1** | **179.9** |

Combined query acceleration is ~2.05x, above the 1.5x cap for the query score.
The remaining SQL-path cost is ~0.3ms/query; the GPU search is ~2.8-3.0ms.

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
the untuned GPU baseline, at recall indistinguishable from perfect on this set
(strict tuner: 1.0000/1.0000; the end-to-end loose figures differ from strict by
at most 0.0001 here); the CPU backend is also the only one of the three that
misses recall@100 (0.9498).

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
"Parameter sweep" above. The line it prints starts with `sorted=1` (assertion
that the returned ids are distance-ascending), then strict `recall@10`,
`recall@10_in100`, and `recall@100`; use `recall@10`, the other is the loose
convention kept for comparison.

## Restart / persistence

After `CREATE VECTOR INDEX`, the snapshot is written during the DDL
(`serialized GPU CAGRA snapshot ... built=true, graph_bytes=256001713`). After a
server restart the index is reloaded by
`ob_cuvs_deserialize` → `seekdb_cuvs_deserialize`
(`deserialized GPU CAGRA snapshot ... n64=250000, graph_bytes=256001713`) and
queries are served from the GPU again with no rebuild.
