# Optional hipVS HNSW build path

Status on 2026-10-28: the modified server and optional bridge/worker were built on AMD gfx1100 / ROCm 7.2.4. Real seekdb SQL CPU and GPU construction, query and restart checks passed on the recorded small dataset. Three measured builds at N=10000, D=128, session ef_search=512 yielded CPU/GPU median end-to-end seconds 2.803622/1.390698, approximately 2.016x; CPU Recall@10 was 0.9953125 and GPU Recall@10 ranged from 0.9953125 to 1.0. Independent same-configuration SQL profiler runs recorded GPU graph-construction kernels. Official data, thresholds, target and acceptance remain unknown; these observations do not establish competition success.

## What changes

The default CPU backend remains VSAG. With `SEEKDB_HNSW_BUILD_BACKEND=hipvs`, the ordinary FP32 HNSW **DDL snapshot** path buffers owned copies of vectors and seekdb VIDs instead of running one CPU insertion per DDL row. At the DDL serialization boundary it builds a CAGRA graph on the AMD GPU, converts it to a complete HNSW with CPU upper-layer construction, and imports the unmodified hnswlib file through VSAG's public `Deserialize(BinarySet)` API.

Dense hipVS ordinal labels are mapped in two public `UpdateId` passes through temporary negative labels to the original nonnegative seekdb VIDs. The imported index then uses normal VSAG queries, filters, serialization, reload and CPU incremental additions. No private VSAG symbols, flattened single-layer import or new seekdb on-disk format are used.

Only the DDL caller explicitly opts into buffering. Online incremental `Add` and other callers retain their existing behavior. A failed GPU build or import does not publish a replacement index and does not silently become a GPU score.

Current GPU support is deliberately explicit:

- 64-bit little-endian Linux, AMD ROCm, FP32 vectors, L2 or inner product.
- Plain `type=hnsw, lib=vsag`, M in [8, 64], no extra-info columns.
- NN-descent graph build without compressed vector storage; complete `HnswHierarchy::CPU` conversion.
- The initial GPU builder requires N > max(128, 4*M). Unsupported small builds fail explicitly instead of reporting CPU work as GPU acceleration.
- The installed hipVS C API must expose CAGRA NN-descent, full CPU HNSW hierarchy conversion with the original dataset, native serialization and resource/stream operations. The bridge uses these public C APIs so the optional build does not require RAFT/RMM C++ development headers.

## Build

This work is based on seekdb `5bce9e06d1281347ad61344e4b01fab9158c58a2`. Build the modified source; the competition template's precompiled seekdb does not automatically contain this DDL implementation.

For a sparse checkout, obtain the complete tree before the full build:

```bash
git sparse-checkout disable
./build.sh release --init --make -j4
```

The repository's standard output is `build_release/src/observer/seekdb`. `-j4` is an example build parallelism, not an experiment CPU-quota declaration.

Build the optional bridge separately after inspecting the actual AMD image. Set `HIPVS_PREFIX` to the installed C API SDK prefix. When the exact installed source commit can be established, record it; otherwise explicitly use `unknown`. CMake records the actual C API and core library SHA256 values in `hipvs-build-manifest.txt`. A binary hash is never used as a source commit. The supplied AMD image has no recoverable source commit and is recorded as `unknown`.

```bash
test -d "${HIPVS_PREFIX:?set the matching hipVS installation prefix}"
cmake -S deps/hipvs -B build_hipvs \
  -DCMAKE_CXX_COMPILER=/opt/rocm/bin/hipcc \
  -DCMAKE_PREFIX_PATH=/opt/rocm \
  -DHIPVS_ROOT="$HIPVS_PREFIX" \
  -DSEEKDB_HIPVS_SOURCE_REVISION=unknown
cmake --build build_hipvs -j4
```

The optional build produces `build_hipvs/libseekdb_hipvs_bridge.so` and `build_hipvs/seekdb_hipvs_worker`. Keep both together by default. GPU construction runs in the worker process, isolating the hipVS and VSAG C++ runtimes; the server does not load ROCm libraries. Input and output are exchanged through private temporary files, and worker startup, transfer, synchronization, conversion, import and cleanup belong in the end-to-end build measurement. The main server does not require these artifacts for its default CPU path.

In the current image, CMake is `/opt/venv/bin/cmake` and `HIPVS_PREFIX=/opt/hipvs`. The installed `libcuvs_c.so` SHA256 is `b63211897276e0a6911bb534fe77c8b0b59f31c50af1df2f108845f06ad46ebf`; `libcuvs.so` is `5ccf012af5923c687cd2d22149740a410ca69e0aa51223a6386ba6fc129c967a`.

## Select the server backend

Configure the **seekdb server process**, not the SQL client:

```bash
export SEEKDB_HNSW_BUILD_BACKEND=hipvs
export SEEKDB_HIPVS_BRIDGE="$PWD/build_hipvs/libseekdb_hipvs_bridge.so"
export SEEKDB_HIPVS_WORKER="$PWD/build_hipvs/seekdb_hipvs_worker"
export SEEKDB_HIPVS_WORKER_TIMEOUT_SECONDS=1800
export SEEKDB_HIPVS_CPU_THREADS=4
export SEEKDB_HIPVS_DEVICE=0
export SEEKDB_HIPVS_MAX_INPUT_BYTES=1073741824
export SEEKDB_HIPVS_MAX_EXPORT_BYTES=1073741824
```

Use an independently allocated data directory and unoccupied ports when starting this binary. For the CPU experiment, use the same binary and CPU resource allocation with `SEEKDB_HNSW_BUILD_BACKEND=cpu`. A thread setting alone does not impose a CPU quota: separately apply and record affinity/cgroup limits consistently for both modes.

The worker path must be absolute; if omitted, seekdb looks for `seekdb_hipvs_worker` next to the bridge. The timeout must be positive and defaults to 1800 seconds. Worker failure or timeout is reported explicitly, with no silent CPU fallback. The two byte limits bound buffered input payload and exported graph bytes. They are not RSS limits; retained capacities, temporary import copies, graph metadata, CPU hierarchy and GPU memory also consume resources. Measure peak host/device use on the actual instance.

`[HIPVS_BUILD]` logs report counts, configured device, bridge version and internal timing, or explicit failure/CPU-path reasons. Internal times exclude some scanning/import/publication work and are never the final speedup measurement. A linked library or successful log alone is not profiler evidence.

## Correct measurement sequence

1. Generate and preserve a dataset, independent queries and exact truth with `hipvs_eval.py prepare`. Its standard-library exhaustive truth implementation is intended for small correctness sets; use a separately verified scalable truth generator for large official sets.
2. Import the same original vectors and document IDs into a new table **without** a vector index. Save import time separately.
3. Verify the target index does not exist. Begin a monotonic wall-clock timer, then execute a post-create vector DDL, for example:

   ```sql
   CREATE VECTOR INDEX hipvs_test_idx ON hipvs_test(embedding)
   WITH (distance=L2, type=hnsw, lib=vsag,
         m=16, ef_construction=200, ef_search=64);
   ```

4. End only after DDL completion, complete indexed-vector count evidence and the same queryability/index-plan check in both modes. Include any work deferred to the first query. A stable top-K or a raw-table `COUNT(*)` alone does not prove complete indexing.
5. Query through seekdb SQL with the preserved query order, original document IDs and the agreed K. Save results, actual execution plan, full build log, configuration and profiler evidence for this exact run. Verify that the modified DDL boundary was reached; an alternate online/refresh path does not establish acceleration by this implementation.
6. Freshly build CPU and GPU indexes at least three times each, alternating modes. Do not reuse an earlier graph. Record end-to-end seconds, both modes' Recall@K, memory and stability.
7. Put the raw observations into the schema documented in [README.md](README.md), then run `hipvs_eval.py validate` and `report`. Compare `median(CPU) / median(GPU)` only. Missing official recall/speedup thresholds produce `unassessed`, not a passing conclusion.

The evaluator validates supplied observations and trace-file integrity; it does not collect SQL timings automatically or certify a profiler trace's contents. A reviewer must confirm the recorded kernels belong to GPU graph construction in this run. The reference Notebook's batch-query speedup is not a build measurement.

## Local checks and remaining acceptance

- Native unit contracts import complete CPU-generated hnswlib fixtures into the real pinned VSAG SDK; these fixture tests alone are not GPU experiments.
- `gpu_bridge_smoke.py` independently builds real L2/IP GPU exports and checks their full graph structure, original FP32 bytes, labels, failure statistics and recovery. `run_gpu_import_test.py` checks all six saved GPU exports through the production public VSAG importer. On N=512, D=32, M=16, 64 independent queries achieved measured Recall@10=1.0 at ef=128/512, with and without filtering; this is a small-sample observation without an official acceptance threshold.
- A separate ROCProfilerV3 bridge run recorded 796 kernel dispatches, including NN-descent local joins/reverse edges and CAGRA sorting/pruning. This proves construction in the independent bridge run, not execution through a completed SQL index build. The trace includes the library's internal half-precision NN-descent preprocessing; original input and exported/stored vectors remain FP32 byte-for-byte. No precision/compression shortcut was enabled by this implementation.
- The current vector Unity group can be syntax-checked with the provided `check_syntax.py` and a matching compiler database. This is not a complete server build or DDL execution test.
- `hipvs_eval.py` has 22 passing standard-library tests; the SQL smoke runner adds 14 tests. The smoke runner labels backend selection as unverified until separate logs/profiler evidence establish it and does not infer complete indexing from table row count.
- On the 2026-10-28 instance, five CPU protocol/import tests and real GPU worker-to-VSAG integration passed. CPU/GPU SQL construction and query tests each ran three times on the same N=10000 dataset; both persisted indexes returned identical 64-query results after restart. The test parameters and raw artifacts are in the local experiment report. Failure cleanup under real SQL workloads, larger datasets, longer stability testing and official-target assessment remain pending. Independent bridge timings, especially profiler-instrumented timings, are never used as competition build scores.

Before competition submission, complete resource-failure cleanup checks, larger-scale tests and official-target assessment. Keep all evidence from the actual implementation. Do not treat local contract tests as the competition's acceptance result.
