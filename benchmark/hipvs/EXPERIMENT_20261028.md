# seekdb × hipVS SQL build experiment (2026-10-28)

These are preliminary measurements, not competition acceptance results. Official dataset sizes, speedup targets, and recall thresholds were unavailable at the time of the experiment.

The modified seekdb 1.4.0.0 was built from base commit `5bce9e06d1281347ad61344e4b01fab9158c58a2` with the changes in this branch. The test instance reported AMD Radeon Graphics (`gfx1100`), AMD EPYC 9334, and ROCm 7.2.4. The CPU quota was 16 cores, process affinity was 0–15, and seekdb used `cpu_count=16` and `memory_budget=16G` in each separate instance. The installed hipVS source revision was unknown; `libcuvs_c.so` SHA-256 was `b63211897276e0a6911bb534fe77c8b0b59f31c50af1df2f108845f06ad46ebf` and `libcuvs.so` SHA-256 was `5ccf012af5923c687cd2d22149740a410ca69e0aa51223a6386ba6fc129c967a`.

The shared dataset contained 10,000 FP32 vectors of dimension 128, 64 independent queries, K=10, seed 42 and L2 distance. IDs were noncontiguous and the exact ground truth used FP64 distance arithmetic. Both modes used M=16, ef_construction=200 and explicit query session `ob_hnsw_ef_search=512`. Each group rebuilt the index three times; GPU runs preceded CPU runs.

| Backend | Build seconds, three runs | Median seconds | Recall@10, three runs |
| --- | --- | --- | --- |
| CPU VSAG | 2.825151, 2.803622, 2.794648 | 2.803622 | 0.9953125, 0.9953125, 0.9953125 |
| hipVS | 1.524351, 1.390698, 1.386574 | 1.390698 | 0.9984375, 1.0000000, 0.9953125 |

The observed median CPU/GPU build ratio was approximately 2.016x. Timing began with the index absent immediately before CREATE INDEX and ended after index-plan readiness, first SQL query, and indexed-count log verification. It includes the worker process, transfers, synchronization, graph conversion and index publication. Data generation, raw import and server startup were recorded separately. The GPU log recorded 10,000 input and indexed elements, `actual_backend=hipvs`, and one GPU submission per build; the CPU group reported `actual_backend=cpu`.

In separate, same-configuration SQL profiler runs, each build recorded 126 GPU kernel dispatches, including NN-descent construction. Instrumented timings were excluded from the comparison above. Both the GPU-built and CPU indexes were restarted and all 64 SQL query results matched their pre-restart results. This is a limited dataset and stability check; larger and official workloads remain to be verified. hipVS internally uses half precision in part of NN-descent preprocessing; this implementation supplies and retains FP32 vectors, which were verified byte-for-byte on a small sample.

Reproduction and data-generation commands, session settings, timing boundaries, and evidence limitations are described in [SQL_BENCHMARK.md](SQL_BENCHMARK.md) and [IMPLEMENTATION.md](IMPLEMENTATION.md). Earlier observations before the query-session fix used the server default `ef_search=64` and must not be confused with the results above.
