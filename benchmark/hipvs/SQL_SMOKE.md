# Real seekdb SQL functional smoke

`sql_smoke.py` runs actual SQL through an installed `mysql` CLI against an
already-started **modified** seekdb server. It does not start a server, use the
competition template binary, choose a server backend, or collect GPU traces.
Python 3.7+ standard library, adjacent `hipvs_eval.py`, and `mysql` are sufficient.

From this directory, after the operator starts the modified server with
`SEEKDB_HNSW_BUILD_BACKEND=cpu` on its dedicated port:

```bash
python3 sql_smoke.py --mode cpu --host 127.0.0.1 --port 2881 --user root --output ./sql-cpu-01
```

After the operator starts the same modified binary with the hipVS backend and
bridge configured, run exactly the same input/quality settings:

```bash
python3 sql_smoke.py --mode gpu --host 127.0.0.1 --port 2881 --user root --output ./sql-gpu-01
```

Use the actual isolated server port; `2881` is an example. On Windows, use
`python` instead of `python3`. `--mysql` accepts an absolute client path. There
is no password argument. Supply any required authentication through the
client's inherited `MYSQL_PWD` environment without printing it. The client
uses `--no-defaults`, TCP, batch/raw output and no column headings. Connection
arguments and raw client diagnostics are not saved. Error artifacts retain
only exit status, MySQL error number and SQLSTATE when present.

Defaults are N=512, D=32, Q=32 independent queries, K=10, seed=42; FP32 input,
noncontiguous positive IDs `101 + 17*i`, L2, M=16, ef_construction=200 and
ef_search=64. Both modes independently prepare byte-identical input/truth
artifacts for the same arguments. The manifest SHA-256 records this identity.
`--mode` labels the intended external server setup; it does not prove which
backend executed. The small-data limits are deliberate because exact truth
uses exhaustive standard-library FP64 arithmetic over stored FP32 values.

The runner creates a UUID-named schema with plain `CREATE DATABASE`. A failed
creation stops immediately. It never drops, reuses or overwrites a schema;
both success and failure retain that schema. The output directory must be
new. It creates a table without a vector index, inserts data in batches,
checks the base-table count and target-index absence, then executes:

```sql
CREATE VECTOR INDEX smoke_vec_idx ON <owned_schema>.vectors(embedding)
WITH (distance=l2, type=hnsw, lib=vsag, m=16, ef_construction=200, ef_search=64);
```

Every query is preceded by an actual `EXPLAIN` and must identify
`smoke_vec_idx` in the object column of a vector-scan operator. An unfamiliar
plan format fails closed and leaves its raw output for review. Each query
must return exactly K unique, positive int64 IDs from the original dataset;
IDs are parsed as integers without floating-point conversion or truncation.
Recall@K uses the independent exhaustive truth and the evaluator's strict
distance-then-ID tie policy. There is no invented recall acceptance threshold.

Artifacts include `data/` with FP32 input, independent queries, IDs, truth and
manifest; one generated `.sql`, successful raw `.stdout.txt`, and safe
`.client.json` metadata per SQL step; and `report.json`. Failed calls do not
persist stdout/stderr because client errors can expose credentials. A timeout
records `timeout_server_outcome_unknown`: the server may continue the DDL.
`schema_created` records a successfully acknowledged creation, not proof of
absence after an unacknowledged/timed-out attempt. Retained resources require
separate operator inspection and explicit cleanup.

Reports always use `measurement: smoke_only`, `actual_backend: unverified`,
`assessment: unassessed`, `indexed_count: null`, and `complete: unverified`.
`functional_checks_completed` means this invocation's result/plan checks
completed, not competition acceptance. `ddl_client_wall_seconds` includes
client launch/connection overhead and DDL response; the first query has its
own client wall time. Neither is the required complete index-build duration.
The base-table `COUNT(*)` is not evidence of indexed-vector completeness.

The source exposes `oceanbase.__all_virtual_vector_index_info`, but its
`statistics` readiness/parameter fields and `sync_info` event counters do not
establish a complete indexed-vector count. This runner does not reinterpret
`snap_cnt` as vector cardinality. Verified full indexing, actual backend,
fallback, GPU kernels, resource use, three alternating fresh builds per mode,
and agreed competition thresholds remain separate acceptance work.

Local contract tests (synthetic client outputs, no real DB):

```bash
python3 -m unittest -v test_sql_smoke test_hipvs_eval
```

The initial 11 tests failed before implementation, then passed. A later
failing regression added safe error-code/timeout metadata. The 14 smoke tests
and unchanged 22 evaluator tests pass on local Python 3.7.0. No CPU or GPU
SQL execution against the modified server has yet been claimed by this file.

Follow-up lifecycle work, once the modified server is available: preserve a
copy of the dataset, insert new noncontiguous IDs, update selected vectors,
delete selected IDs and recompute exact truth after each change; then restart
the same modified server/data directory and repeat result/plan checks. These
mutations/restarts are not implemented or executed by this smoke runner.
