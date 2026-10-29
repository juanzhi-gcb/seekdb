# AMD CAGRA build tuning (V5 baseline)

This branch tunes the GPU CAGRA build parameters in
`tools/hipvs_bridge/seekdb_cuvs_bridge.c` on top of the frozen V5 contest
baseline `634888d9b037153fa143d2c913ba4b25503e1d62`.

## Change

The V5 bridge builds every CAGRA index with `graph_degree=128`,
`intermediate_graph_degree=256` and `nn_descent_niter=100`. NN-descent on a
256-degree intermediate graph is the dominant cost of the offline DDL build.

The tuned bridge keeps the final `graph_degree=128` (query behavior and graph
quality are unchanged) and selects the intermediate graph and iteration count
by dimension:

```c
const bool low_dim = (dim <= 128);
h->ip->intermediate_graph_degree = low_dim ? 128 : 192;
h->ip->nn_descent_niter = low_dim ? 30 : 40;
```

Lower-dimensional vectors converge with a smaller intermediate graph and fewer
refinement passes; higher-dimensional vectors keep a larger graph and more
iterations to preserve recall.

## Environment

- GPU: AMD Radeon PRO W7900 (gfx1100, 48 GB), ROCm 7.2.4
- CPU: 24-core affinity (0-23), seekdb `cpu_count=24`, `memory_limit=64G`
- seekdb: V5 `634888d9b`, built with `OB_BUILD_CUVS=ON`,
  `OB_BUILD_CUVS_TRACE=OFF`, bridge built by `tools/hipvs_bridge/build.sh`
- Dataset import, server startup and dataset generation are excluded from all
  timings; timing starts immediately before `CREATE VECTOR INDEX ... lib=cuvs`
  and ends after the first SQL query returns.

## Measurements

### SIFT250K (real SIFT1M first 250K, L2, 100 queries)

| Build | End-to-end | Speedup | Recall@10 | Recall@100 |
| --- | --- | --- | --- | --- |
| V5 baseline (128/256/100) | 20.08 s | 1.00x | 1.0000 | 0.9999 |
| Tuned (128/128/30) | 8.02 s | 2.50x | 1.0000 | 1.0000 |

### 250K x 256 synthetic, uniform L2 (50 queries)

| Build | End-to-end | Speedup | Recall@10 | Recall@100 |
| --- | --- | --- | --- | --- |
| V5 baseline (128/256/100) | 23.10 s | 1.00x | 0.916 | 0.8636 |
| Tuned (128/192/40) | 12.95 s | 1.78x | 0.926 | 0.8616 |

Recall deltas are within the contest's 0.005 relative gate on this local
dataset: +0.010 / -0.0020. The hidden 250K x 256 dataset is not available
locally; this synthetic result is a local check, not an official score.

### 250K x 128 synthetic, uniform L2 (100 queries)

| Build | End-to-end | Speedup | Recall@10 | Recall@100 |
| --- | --- | --- | --- | --- |
| V5 baseline | 20.79 s | 1.00x | 0.989 | 0.9670 |
| Tuned (128/128/30) | 7.23 s | 2.88x | 0.990 | 0.9647 |

## Reproduction

Build:

```bash
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so
./build.sh release --init \
  -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so \
  --make -j64
```

SQL:

```sql
CREATE TABLE t(id BIGINT PRIMARY KEY, v VECTOR(128));
-- import 250K vectors
CREATE VECTOR INDEX idx ON t(v)
  WITH (distance=l2, type=hnsw, lib=cuvs);
```

Time from the statement above through the first
`ORDER BY l2_distance(v, '...') APPROXIMATE LIMIT 10` query. Use the same
table, vectors and index parameters for the baseline and tuned builds, and
drop/recreate the index between runs.

## Scope and limitations

- No change to seekdb query paths or `lib=vsag`; only GPU build parameters.
- The CAGRA build still runs on the GPU through hipVS; there is no CPU
  fallback.
- Local absolute times vary; official scoring compares the candidate against
  the frozen V5 baseline in the same evaluation round.
