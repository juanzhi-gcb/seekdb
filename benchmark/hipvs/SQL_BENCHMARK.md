# Real SQL end-to-end build observations

`sql_benchmark.py` measures actual SQL on an **already running modified seekdb
server**. It does not start a server, select its backend, upgrade dependencies,
connect to a cloud account, or collect a GPU profiler trace. Python 3.7+ standard
library, the adjacent evaluator and smoke modules, and an installed `mysql` CLI
are sufficient. There are no measured SQL performance results in this document.

The output is deliberately an observation report, separate from the evaluator's
validated experiment schema. Successful collection means
`status: performance_observation_only`, `acceptance: acceptance_unassessed`,
`assessment: unassessed`, and `evaluator_validated: false`. No official speedup
or Recall threshold is invented. A backend log does not prove GPU kernels.

## Operator prerequisites and example commands

Build the modified server first. Use the same binary, dataset, quality settings,
CPU quota/affinity, thread settings and environment for CPU and GPU. Backend
selection belongs to the server process: `SEEKDB_HNSW_BUILD_BACKEND=cpu` or
`hipvs` with the actual bridge configured. Use separate owned server data
directories/ports and preserve the exact source and library identities. Record
actual CPU quotas, `cpu_count`, affinity, memory and environment separately; this
runner does not infer those values from thread settings.

**Only this runner may perform vector DDL in the selected server process/log
during collection. Disable log rotation, copy/truncation and rewriting.** A log
record has no schema/index/run identity. Correlation is conditional on this
isolated single-build window, and the report records that limitation explicitly.
Do not point `--server-log` to another process's log or a previous run's copy.
The log must be locally readable and already exist before SQL begins.

After the operator has started the CPU server, an example invocation is:

```bash
python3 sql_benchmark.py --mode cpu --host 127.0.0.1 --port 2881 \
  --server-log /absolute/path/to/cpu-server/log/seekdb.log \
  --output ./sql-cpu-observations
```

Against the separately configured GPU server, reuse the exact preserved dataset:

```bash
python3 sql_benchmark.py --mode gpu --host 127.0.0.1 --port 2882 \
  --server-log /absolute/path/to/gpu-server/log/seekdb.log \
  --dataset ./sql-cpu-observations/data/manifest.json \
  --output ./sql-gpu-observations
```

Paths and ports above are examples, not evidence of a running instance. On
Windows use `python` and Windows paths. `--mysql` may specify an executable path.
Supply authentication through the inherited `MYSQL_PWD` environment if needed;
there is no password argument. The reused `MysqlClient` sends generated SQL via
stdin, never invokes a shell, and persists safe status/error-code metadata
instead of connection arguments, passwords or raw client error diagnostics.

Each invocation performs at least three fresh builds of one externally selected
mode. These two example calls collect separate CPU and GPU groups; they do not
alternate the modes. If alternating CPU/GPU trial order is required by the
experiment protocol, arrange it externally and record the actual order. The
runner does not switch or restart server processes, and it rejects
`--repetitions` below three.

## Dataset and settings

Defaults are N=10000, D=128, Q=16 independent queries, K=10, seed=42, M=16,
ef_construction=200, ef_search=128 and three repetitions. Each EXPLAIN and
query connection explicitly sets the session `ob_hnsw_ef_search` to the
requested value; the index declaration alone does not select the query session
value. Earlier 2026-10-28 observations before this fix used the server default
64 despite reporting the index declaration, and are retained only as historical
results. Generated IDs are the
original noncontiguous positive int64 values `101 + 17*i`. Input and queries are
the evaluator's stored FP32 values; SQL decimal representations round-trip to
those FP32 values. No data compression or precision conversion is introduced.

This initial runner supports `l2_squared` (default) and `l2` manifests. Both use
SQL `distance=l2` and `l2_distance` because their exact nearest-neighbor ordering
is identical. Other metrics are rejected before schema creation. M is limited
to [8,64], dimensions to 4096, and N must exceed `max(128, 4*M)` for the same
supported input range in both modes.

Dataset generation and exhaustive independent exact truth use
`hipvs_eval.prepare_dataset`. Generated workloads are limited to 100 million
component distances. The default requires about 20.5 million. Larger workloads
can supply `--dataset /path/to/manifest.json`; its shape, seed and metric take
precedence over generation flags. The runner checks hashes, sizes, formats,
finite FP32 vectors, original unique int64 IDs and truth structure through the
existing evaluator, copies the explicitly selected manifest bytes to
`data/manifest.json` and the adjacent fixed data files into the new output
folder, then validates the copy again. A selected manifest may have a nondefault
filename; a sibling `manifest.json` never replaces it. Existing truth is trusted as documented by the
evaluator; it is not independently recomputed on every invocation. A separately
verified scalable truth generator remains future work.

Only 128 vector rows are held per insertion batch. Queries, IDs, truth and the
evaluator's validation structures still consume host memory proportional to
their dataset sizes. External manifests remove the generation limit, not all
memory costs. Keep the complete manifest and file hashes identical in CPU/GPU
comparisons; the report preserves `manifest_sha256` and common `config`.

## Timing and SQL lifecycle

The tool creates one UUID-named owned schema with plain `CREATE DATABASE`; it
never accepts a user schema name, reuses an existing schema, or automatically
drops a database. Existing output directories are rejected. Success and failure
both retain the owned schema for inspection.

1. Generate or validate/copy the FP32 vectors, original IDs, independent queries
   and exact truth. Record `generation_seconds` and
   `dataset_preparation_seconds` separately. External data have generation time
   zero, with validation/copy time in preparation time. Server startup is
   external, so `server_startup_seconds` is null, not an invented zero.
2. Create a table without a vector index, import the raw vectors/IDs in batches
   of at most 128 rows, and check the base-table row count. Record
   `raw_import_seconds` separately, including table creation and import checking.
   This count verifies raw import only; it is never indexed-count evidence.
3. Before each round, drop only the acknowledged index built by this invocation
   in its own schema (rounds two and later). Verify that the target vector index
   is absent. Save the current log byte offset and file identity.
4. Start a monotonic wall-clock timer immediately before issuing actual
   `CREATE VECTOR INDEX benchmark_vec_idx ... WITH (distance=l2, type=hnsw,
   lib=vsag, m=..., ef_construction=..., ef_search=...)`. Every round reconstructs
   the index from the original imported table; it never reuses an earlier graph.
5. Wait for an actual `EXPLAIN` that names `benchmark_vec_idx` in the object
   column of a vector-index-scan operator. Execute the first actual top-K query
   and verify K legal, unique original IDs. Capture and validate the current
   finalize log record and its actual `GetNumElements` count. Stop the timer
   only after these checks. DDL, client launch/connection overhead, polling,
   index availability, deferred first-query work and log readiness checks are
   all included. CPU and GPU use exactly the same boundaries.
6. Outside the timer, execute all Q quality queries in preserved order,
   including query zero again. Require exactly K unique positive int64 IDs from
   the dataset, compute Recall@K against the independent exact truth, and
   recheck the log window for late duplicate/error records. Only a fully
   successful round enters the top-level `raw_seconds` list.

`end_to_end_seconds` is a direct monotonic duration in seconds. All successful
raw per-round times are preserved in order, including slow rounds. The report
also gives their median; it never selects the minimum or converts
`bridge_wall_ms`, `gpu_build_ms` or any internal bridge stage into the SQL
end-to-end measurement. For a later comparison use median(CPU)/median(GPU),
with complete protocol and evidence review.

## Finalize log checks and remaining evidence boundary

The modified source logs actual `index_->GetNumElements()` at finalization.
The runner requires exactly one complete `[HIPVS_BUILD]` record in its window:

- CPU: `actual_backend=cpu`, `reason=cpu_selected`,
  `gpu_build_submissions=0`, and `indexed_count == N`.
- GPU: `[HIPVS_BUILD] success`, `actual_backend=hipvs`,
  `input_count == N`, `indexed_count == N`, and a positive
  `gpu_build_submissions` count.

Any build error, unsupported path, empty build, CPU fallback, wrong count,
missing required field or multiple build records fails closed. Missing log
output is polled only until the readiness deadline. An incomplete trailing
build record remains unverified. The parser accepts ordinary `key=value` and
`key: value` scalar formatting, including quoted scalars; no real SQL logger
rendering has yet been certified by the offline tests. Unfamiliar formatting
must be reviewed against actual retained logs, not loosened into a fake match.

Each round preserves the raw log bytes from its starting offset through its
last check, SHA-256, file identity, starting offset and the exact matched raw
record. File replacement, shortening or rewriting of the preceding 4096-byte
anchor fails closed. A replacement/truncation occurring during capture is
also checked. Identical-byte rewrites cannot be distinguished by a reader;
therefore disabling rotation/rewrites is an explicit operator prerequisite.
If a log window becomes invalid, the report records that capture was invalid
or unavailable; inspect the original server and rotated logs separately. The
tool does not silently treat an old log as this round's evidence.

These logs establish a source-instrumented backend/count observation within
the stated isolation assumption. They do not authenticate a GPU profiler trace.
Every run has `gpu_evidence: null`. A future reviewer must correlate actual
GPU construction kernels, device and build intervals with **these SQL runs**
before constructing evaluator records. An older independent bridge trace
cannot supply that evidence. Source/library identities, resource quotas,
memory observations, cache/workload policy and any official thresholds also
remain external evidence requirements.

## Artifacts and failure handling

The new output directory contains `data/`, `report.json`, generated `.sql`,
successful `.stdout.txt`, safe `.client.json` metadata per invocation, and
`round_NNNN.server.log` files. SQL/plan/result artifacts come from the real client
when the runner is used against a server. Synthetic test outputs are explicitly
TEST ONLY and are removed with their temporary directories.

The default per-client timeout is 1800 seconds. `--ready-timeout` defaults to
300 seconds after DDL acknowledgment for plan readiness, first query and log
evidence; client timeouts during this phase are capped to the remaining budget.
`--poll-interval` defaults to one second. Query/client errors stop immediately.
A client timeout can leave the server still building: the reused client records
`timeout_server_outcome_unknown`, and the report marks the current round
`failed_server_outcome_may_be_unknown`. The runner neither retries that DDL nor
drops/rebuilds the uncertain index. It stops the invocation, retains artifacts
and schema, and requires separate operator inspection/cleanup.

On any error `status` is `failed`; inspect per-round status as well as raw
durations. A time already observed before a failed quality check is retained
within that failed round but never admitted to top-level `raw_seconds`.
Completed earlier rounds stay visible; a partial invocation is not a valid
three-round result. CLI exit code is 0 for completed observation collection,
2 for controlled validation/client/filesystem failures. A successful exit is
not competition acceptance or proof of profiler execution.

## Local verification

From this directory:

```bash
python3 -m unittest -v test_sql_benchmark test_sql_smoke test_hipvs_eval
python3 sql_benchmark.py --help
```

Offline tests cover measurement boundaries, first query and polling inclusion,
three fresh indexes with only owned drops, byte-identical CPU/GPU inputs/config,
128-row raw import batches, existing manifests, exact quality validation,
backend/count/fallback errors, ambiguous and partial log records, append windows,
rotation/truncation, readiness/client failure stopping and schema retention.
They use TEST ONLY client/log fixtures and do not start or connect to a server.
Actual modified-server SQL integration, observed log rendering and SQL-bound
GPU profiler evidence remain unverified until the operator runs them.
