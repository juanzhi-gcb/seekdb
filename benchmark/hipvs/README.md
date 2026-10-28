# hipVS build measurement helpers

This directory is portable to `seekdb/benchmark/hipvs/`. Python 3.7+ and its
standard library are sufficient; NumPy, ROCm and a database are not needed for
data preparation or record validation.

These helpers prepare data and audit externally collected measurements. They
do **not** run SQL, construct an index, execute a profiler, or prove that an
instrumented database used the intended build path. No measured CPU/GPU report
is included. Every synthetic fixture in the unit tests is labeled `TEST ONLY`.

## Commands

From this directory (or prefix the script path appropriately):

```powershell
python hipvs_eval.py --help
python hipvs_eval.py prepare --output ./data --rows 1000 --queries 20 --dimensions 32 --k 10 --seed 42 --metric l2_squared
python hipvs_eval.py validate --manifest ./data/manifest.json --records ./experiment/raw.json --output ./experiment/validated.json
python hipvs_eval.py report --manifest ./data/manifest.json --records ./experiment/raw.json --output ./experiment/report.json
```

`validate` and `report` perform the same complete validation and emit the same
JSON report. Omitting `--output` writes JSON to stdout. Relative profiler paths
resolve against the directory containing `--records`; `--evidence-root PATH`
overrides that base. Absolute artifact paths are supported. Dataset files resolve
against the manifest directory. Invalid input returns exit code **2** with an
error on stderr; valid input returns **0**, including a valid report whose target
assessment is `fail` or `unassessed`. Inspect `assessment` separately.

The preparation directory must be empty or absent. Preparation never overwrites
an existing dataset. The exhaustive standard-library truth calculation takes
O(N × Q × D) arithmetic and O(N × D + Q × D) matrix storage (plus a sorted N-row
distance list per query). Use small datasets for correctness checks. This is
not an optimized million-vector ground-truth generator.

## Prepared data and truth

| File | Format |
| --- | --- |
| `vectors.f32` | N × D row-major FP32, little-endian, no header |
| `queries.f32` | Q × D row-major FP32, little-endian, no header |
| `ids.i64` | N signed little-endian int64 document IDs, no header |
| `truth.json` | `{"ids": [[...]], "distances": [[...]]}`, Q rows with K entries each |
| `manifest.json` | Version, seed, shape, metric, formats, tie policy and file SHA-256/byte sizes |

IDs are `101 + 17*i`, positive and noncontiguous. Database insertion and returned
query IDs must preserve these original IDs, not replace them with row offsets.
Queries are generated independently after the index vectors from a seeded
`random.Random` stream. Uniform values in [-1, 1] are explicitly rounded to
FP32 before files or truth are computed. The manifest identifies the generator
as `python_random_uniform_fp32_v1`. Identical arguments produce identical files;
paths and current timestamps are excluded from the manifest.

`shape` is `{"rows": N, "queries": Q, "dimensions": D, "k": K}`. All sizes must
be positive and K ≤ N. The seed is an integer. The manifest's `files` maps the
four fixed data filenames to `{"sha256": "...", "bytes": integer}`. Record the
SHA-256 of **the manifest file bytes** as each run's `manifest_sha256`:

```powershell
(Get-FileHash ./data/manifest.json -Algorithm SHA256).Hash.ToLowerInvariant()
```

Supported distance definitions, evaluated using FP64 arithmetic and `math.fsum`
over the stored FP32 values:

| `metric` | Ascending distance used for exact truth |
| --- | --- |
| `l2_squared` | Σ(xᵢ − qᵢ)² |
| `l2` | sqrt(Σ(xᵢ − qᵢ)²) |
| `cosine` | 1 − dot(x,q)/(norm(x)norm(q)); zero vectors rejected |
| `inner_product` | −dot(x,q), so a larger dot product ranks first |

The public `exact_truth` API checks that distance results and cosine/dot-product
intermediates are finite before sorting or clamping. Arithmetic outside the
supported FP64 range raises `ValueError`; this includes L2 squaring overflow.
It does not silently convert overflow into a valid truth distance. Inputs
prepared by this helper use bounded FP32 values.

Truth sorts by `(distance, original_document_ID)` and keeps K entries. Its
`tie_policy` is `strict_distance_then_ascending_id`. Recall@K averages
`|returned_IDs ∩ canonical_truth_IDs| / K` across all queries. A different equally
distant ID at the K boundary receives no credit under this **strict canonical**
policy. This is deterministic and explainable but can penalize a mathematically
equivalent tie result; do not claim it is a tie-tolerant metric. Record the policy
when comparing to another evaluator or an official threshold. L2 and squared L2
give the same ordering, while their distance values differ.

Validation checks file hashes, sizes, finite vector values, valid unique int64
IDs, and truth shape/IDs/distances. It does not independently recompute exhaustive
truth during every report: the prepared manifest/truth must be trusted and
preserved. Hashes establish artifact integrity, not authenticity.

## Raw experiment JSON

The root object is:

```json
{
  "schema_version": 1,
  "label": "MEASURED",
  "runs": []
}
```

`label` is `MEASURED` only for real runs, or `TEST ONLY` for synthetic data. The
tool preserves that label in its output; it cannot infer whether a collector is
truthful. `runs` must contain **at least three CPU and three GPU runs**, each with
a unique nonempty `run_id`. One file represents one dataset/configuration pair;
use separate files for different configurations. Run order is retained.

Every run requires the following fields. This is a **TEST ONLY schema example**,
not measured performance. Placeholder hashes and empty `results` intentionally
make it invalid until replaced by real collected values:

```json
{
  "run_id": "gpu-01",
  "mode": "gpu",
  "manifest_sha256": "REPLACE_WITH_ACTUAL_MANIFEST_SHA256",
  "config": {
    "metric": "l2_squared",
    "dimensions": 32,
    "k": 10,
    "quality": {
      "M": 16,
      "ef_construction": 200,
      "ef_search": 64,
      "gpu_graph_degree": 32
    }
  },
  "cpu_quota": {"cores": 4, "affinity": "0-3"},
  "source_versions": {
    "seekdb": "REPLACE_WITH_EXACT_COMMIT",
    "hipvs": "REPLACE_WITH_EXACT_VERSION_OR_COMMIT"
  },
  "measurement": "index_build_end_to_end",
  "end_to_end_seconds": 1.0,
  "fresh_index": true,
  "all_build_work_included": true,
  "actual_backend": "hipvs",
  "cpu_fallback": false,
  "index": {
    "ready": true,
    "queryable": true,
    "complete": true,
    "indexed_count": 1000,
    "query_plan_uses_index": true
  },
  "results": [],
  "gpu_evidence": {
    "kind": "profiler_kernel_trace",
    "artifact": "traces/gpu-01.trace.json",
    "sha256": "REPLACE_WITH_ACTUAL_TRACE_SHA256",
    "device": "REPLACE_WITH_ACTUAL_AMD_GPU_AND_DEVICE_ID",
    "build_run_id": "gpu-01",
    "kernels": [
      {"name": "REPLACE_WITH_OBSERVED_BUILD_KERNEL", "launch_count": 1, "duration_ns": 100, "phase": "build"}
    ]
  }
}
```

CPU runs use `mode: "cpu"`, `actual_backend: "cpu"`, and do not require
`gpu_evidence`. GPU runs require `mode: "gpu"`, `actual_backend: "hipvs"`, and
the evidence object. `cpu_fallback` must be false for both modes. GPU hierarchy
conversion may intentionally perform CPU work as part of the configured hipVS
pipeline; this is different from silently falling back to the ordinary CPU
index builder. All configured pipeline stages still count toward elapsed time.

`config`, `cpu_quota`, and `source_versions` must match across **every** run.
Include both modes' settings in the shared quality object; it must be nonempty.
The tool compares this object exactly rather than inferring equivalent quality
from parameter names. The common metric, dimensions and K must match the
manifest. Quota requires positive numeric `cores` and a nonempty affinity
description; record actual cgroup limits, thread settings and CPU model in
additional fields if relevant. Source versions must identify both seekdb and
hipVS, including for the baseline run from the same experiment environment.

`results` must contain one list for every query, in manifest query order. Each
list must contain exactly K distinct positive int64 IDs present in `ids.i64`.
Duplicate, missing, floating-point, boolean, negative, zero or unknown IDs fail
validation. The returned list ordering does not affect set Recall@K. Collect
these results through the actual seekdb SQL entry point after the index becomes
ready. The validator reads the supplied results; it does not connect to SQL.

The elapsed time must be a finite positive JSON number. Booleans, strings,
zero, negative values, NaN and infinities are rejected. The measurement type must
be `index_build_end_to_end`; query throughput and GPU-only stage time cannot
serve as a build measurement. Duplicate JSON object keys are rejected. Excessive
JSON nesting is rejected with a controlled validation error (CLI exit code 2).

Additional JSON fields are preserved in `raw_runs`: recommended fields include
host/CPU/GPU models, RAM/VRAM and peak usage, OS/ROCm/hipVS versions, compile
flags, CPU/GPU clock settings, SQL text, SQL explain/ready/count artifacts,
snapshot identity and scan coverage, profiler command, run start/end timestamps,
and optional stage durations in explicit units. Keep raw logs alongside the
JSON. Optional stage durations are descriptive and never replace the recorded
end-to-end time.

## External measurement protocol

1. Use the same AMD machine, database revision, input snapshot, query set,
   distance definition, FP32 storage, quality configuration, CPU quota and
   affinity for both modes. Freeze background workload and document cache and
   warm-up policy. Record the actual host and software environment.
2. Import all original vectors and original IDs **before** timing. Ensure the
   target index does not exist; drop/recreate it for every timed run. Never
   reuse a graph from a preceding run. Alternate CPU and GPU trials, with at
   least three independent rounds per mode.
3. Start a monotonic wall clock immediately before the operation initiating
   index construction. Include scan/collection, host/device transfers, graph
   construction, synchronization, full HNSW conversion, mapping, persistence
   and publication that belong to that build. Avoid timing only the hipVS API
   call or an asynchronous enqueue operation.
4. Stop only once the complete index is ready for normal seekdb SQL querying.
   Use the same ready/queryability checks for both modes. Include deferred or
   lazy work triggered by initial queries. Verify the formal index completion
   state, total indexed vector count, full scan/VID mapping coverage, and an SQL
   explain plan that uses the intended index. Stable top K alone cannot prove
   that an index is complete.
5. Collect all Q × K query results and validate them against the independent
   truth. Keep readiness/count/explain evidence and all per-round wall-clock
   seconds, including slow rounds. An incomplete index, invalid results, CPU
   fallback, or missing evidence invalidates the experiment.
6. Capture profiler kernel activities from the actual build interval for each
   GPU run. Save the raw trace and compute its SHA-256 after collection. Keep
   build/run correlation and device identity. Record transfers and conversion
   stages separately if useful, while retaining the full wall-clock measurement.

The validator verifies declarations and artifacts, not the physical execution
of these steps. Review the collector, raw SQL logs, index state, measurement
boundaries, configuration and profiler contents before accepting a result.

## GPU evidence limitations

Every GPU run requires structured `profiler_kernel_trace` evidence with a device,
matching `build_run_id`, and at least one named build kernel with positive
integer launch count and positive finite duration in nanoseconds. The artifact
must exist, be nonempty and match its declared SHA-256. A library name, linked
`libhipvs.so`, a log string saying "GPU", or query-only events are insufficient.

The helper **does not parse vendor profiler formats or authenticate events**.
Even a structurally valid object could misdescribe its trace. A reviewer must
open the actual trace, correlate the build interval/device/run, and establish
that hipVS graph construction executed GPU kernels. Merely passing the schema
and file hash checks is not proof of GPU computation. Every report therefore
contains `gpu_evidence_manual_review_required: true` and limits its assessment
to numerical target checks conditional on protocol/profiler review.

## Report and optional official targets

The report keeps `raw_runs` and the original `raw_seconds` lists, plus:

- `median_seconds.cpu` and `median_seconds.gpu`;
- `speedup = median(cpu seconds) / median(gpu seconds)`;
- per-round mean query `recall_at_k.cpu` and `recall_at_k.gpu`;
- data/metric/tie identity, original label, and manual-review requirement;
- `assessment`: `unassessed`, `pass`, or `fail`.

This is a ratio of the two independent medians, **not** a median of paired
ratios. The synthetic test CPU [1, 2, 100], GPU [1, 4, 5] therefore produces
2/4 = 0.5. Those numbers are **TEST ONLY**, not an experiment result.

Until official speedup and recall targets are provided, omit `--targets`.
Assessment is always `unassessed` when either target or its source is absent.
The tool supplies no default acceptance threshold. To evaluate user-supplied
official targets, create a JSON file with all three keys:

| Key | Requirement |
| --- | --- |
| `speedup_min` | Finite positive number from the official rules |
| `recall_min` | Finite number in [0, 1] from the official rules |
| `source` | Nonempty reference identifying the supplied official target |

```powershell
python hipvs_eval.py report --manifest ./data/manifest.json --records ./experiment/raw.json --targets ./official-targets.json --output ./experiment/report.json
```

With complete valid targets, numerical `pass` requires speedup ≥ the target
and **every CPU and GPU round's recall** ≥ the recall target. Otherwise the
numerical assessment is `fail`. This does not certify profiler authenticity,
SQL lifecycle correctness or official competition acceptance. Synthetic output
retains `TEST ONLY` even when a test intentionally evaluates numerical targets.

## Python API

```python
from hipvs_eval import prepare_dataset, exact_truth, recall_at_k, sha256_file, validate_experiment

manifest = prepare_dataset("data", rows=100, queries=5, dimensions=8, k=5, seed=42)
# exact_truth(vectors, queries, ids, k, metric) -> (truth_ids, truth_distances)
# recall_at_k(results, truth_ids, k, allowed_ids=None) -> float
# validate_experiment(records, manifest_path, evidence_root=None, targets=None) -> dict
```

`prepare_dataset` and `validate_experiment` use keyword-only options after the
first positional arguments. Invalid values raise `ValueError`; filesystem
failures may raise `OSError`. `allowed_ids` should be the complete set of
dataset IDs when using `recall_at_k` independently; without it, the helper
cannot determine whether an otherwise valid ID exists in the dataset.
`validate_experiment` always supplies the full allowed ID set.

## Tests and local verification

```powershell
python -m unittest discover -s amd-hipvs/evaluation -v
# Or, from this directory:
python -m unittest -v test_hipvs_eval
```

On 2026-10-26, Python 3.7.0 standard-library unittest completed **22 tests, OK**.
The initial 14 tests were executed before implementation and failed because
the module did not yet exist. Additional regression cases exposed malformed
mode objects and boolean schema versions; those were fixed and all 18 passed.
Four further failing regressions reproduced distance-intermediate overflow,
overflow in even-count timing medians, and excessive JSON nesting in API/CLI
inputs. These were fixed, and all 22 tests passed. Even-count positive medians
use `low + (high - low) / 2` to avoid overflow in `low + high`.
Coverage includes deterministic binary data, metric and tie semantics,
noncontiguous IDs, malformed/duplicate/missing results, finite positive times,
separate group medians, mismatched data/config/quota/version, insufficient
rounds, incomplete indexes, fallback/reused builds, evidence structure and file
hashes, target handling, duplicate JSON keys, and prepare/validate/report CLI
execution. The CLI exercise creates only temporary **TEST ONLY** records and
reports, which are removed afterwards. No AMD GPU, SQL integration, or real
performance acceptance was tested by this suite.
