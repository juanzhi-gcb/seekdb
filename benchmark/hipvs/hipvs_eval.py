"""Portable data preparation and validation for hipVS index-build measurements.

This module does not execute SQL or run a profiler. Evidence validation checks
provenance fields and artifact integrity; a human must review profiler contents.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import sys

METRICS = ("l2_squared", "l2", "cosine", "inner_product")
TIE_POLICY = "strict_distance_then_ascending_id"


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _integer(value, minimum=1):
    return type(value) is int and minimum <= value <= (1 << 63) - 1


def _positive(value):
    return type(value) in (int, float) and math.isfinite(value) and value > 0


def _text(value):
    return isinstance(value, str) and bool(value.strip())


def _json_bytes(value):
    try:
        return (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")
    except RecursionError as error:
        raise ValueError("JSON nesting exceeds the supported depth") from error


def _load_json(path):
    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            _require(key not in result, f"duplicate JSON key: {key}")
            result[key] = value
        return result

    def invalid_constant(value):
        raise ValueError(f"nonfinite JSON value: {value}")

    try:
        return json.loads(Path(path).read_text(encoding="utf-8"),
                          object_pairs_hook=unique_pairs, parse_constant=invalid_constant)
    except RecursionError as error:
        raise ValueError("JSON nesting exceeds the supported depth") from error


def sha256_file(path):
    """Hash an artifact without loading the whole file into memory."""
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _distance(a, b, metric):
    try:
        if metric in ("l2_squared", "l2"):
            squared = math.fsum((x - y) ** 2 for x, y in zip(a, b))
            distance = squared if metric == "l2_squared" else math.sqrt(squared)
        else:
            dot = math.fsum(x * y for x, y in zip(a, b))
            _require(math.isfinite(dot), "distance dot product must be finite")
            if metric == "inner_product":
                distance = -dot  # Smaller distance is better.
            else:
                norm_product = math.fsum(x * x for x in a) * math.fsum(y * y for y in b)
                _require(math.isfinite(norm_product), "cosine norm product must be finite")
                norm = math.sqrt(norm_product)
                _require(norm > 0, "cosine distance has a zero vector or norm underflow")
                similarity = dot / norm
                _require(math.isfinite(similarity), "cosine similarity must be finite before clamping")
                distance = 1.0 - max(-1.0, min(1.0, similarity))
        _require(math.isfinite(distance), "distance result must be finite")
        return distance
    except OverflowError as error:
        raise ValueError("distance arithmetic overflow; use representable inputs") from error


def exact_truth(vectors, queries, ids, k, metric="l2_squared"):
    """Exhaustive FP64 distances over supplied (normally quantized FP32) data.

    Ties use ascending original ID; Recall uses that strict canonical top K.
    """
    _require(metric in METRICS, "unsupported metric")
    _require(isinstance(vectors, list) and vectors and isinstance(queries, list) and queries,
             "vectors and queries must be nonempty matrices")
    _require(_integer(k) and k <= len(vectors), "k must be in [1, rows]")
    _require(isinstance(ids, list) and len(ids) == len(vectors)
             and all(_integer(i) for i in ids) and len(set(ids)) == len(ids),
             "IDs must be unique positive int64 values")
    _require(isinstance(vectors[0], list) and vectors[0], "invalid vector dimensions")
    dimensions = len(vectors[0])
    for row in vectors + queries:
        _require(isinstance(row, list) and len(row) == dimensions
                 and all(type(v) in (int, float) and -sys.float_info.max <= v <= sys.float_info.max for v in row),
                 "vectors must have equal dimensions and finite FP64-representable values")
    truth_ids, truth_distances = [], []
    for query in queries:
        nearest = sorted((_distance(vector, query, metric), doc_id)
                         for vector, doc_id in zip(vectors, ids))[:k]
        truth_ids.append([doc_id for _, doc_id in nearest])
        truth_distances.append([distance for distance, _ in nearest])
    return truth_ids, truth_distances


def prepare_dataset(output, *, rows, queries, dimensions, k, seed=42, metric="l2_squared"):
    """Write deterministic row-major little-endian FP32 vectors and int64 IDs.

    The standard-library exhaustive truth implementation is intended for small
    correctness datasets; its time complexity is O(rows * queries * dimensions).
    """
    for name, value in [("rows", rows), ("queries", queries), ("dimensions", dimensions), ("k", k)]:
        _require(_integer(value), f"{name} must be a positive int64")
    _require(type(seed) is int, "seed must be an integer")
    _require(k <= rows and 101 + 17 * (rows - 1) <= (1 << 63) - 1, "invalid k or ID range")
    _require(metric in METRICS, "unsupported metric")
    output = Path(output)
    _require(not output.exists() or not any(output.iterdir()), "output directory must be empty")
    rng = random.Random(seed)

    def matrix(count):
        return [[struct.unpack("<f", struct.pack("<f", rng.uniform(-1.0, 1.0)))[0]
                 for _ in range(dimensions)] for _ in range(count)]

    vectors, query_vectors = matrix(rows), matrix(queries)
    ids = [101 + 17 * i for i in range(rows)]
    truth_ids, truth_distances = exact_truth(vectors, query_vectors, ids, k, metric)
    output.mkdir(parents=True, exist_ok=True)
    for name, data in [("vectors.f32", vectors), ("queries.f32", query_vectors)]:
        with (output / name).open("wb") as stream:
            for row in data:
                stream.write(struct.pack(f"<{dimensions}f", *row))
    with (output / "ids.i64").open("wb") as stream:
        for doc_id in ids:
            stream.write(struct.pack("<q", doc_id))
    (output / "truth.json").write_bytes(_json_bytes({"ids": truth_ids, "distances": truth_distances}))
    manifest = {
        "schema_version": 1, "seed": seed, "generator": "python_random_uniform_fp32_v1",
        "shape": {"rows": rows, "queries": queries, "dimensions": dimensions, "k": k},
        "metric": metric, "vector_format": "little_endian_float32_row_major",
        "id_format": "little_endian_signed_int64", "truth_arithmetic": "FP64_math_fsum",
        "tie_policy": TIE_POLICY,
        "files": {name: {"sha256": sha256_file(output / name), "bytes": (output / name).stat().st_size}
                  for name in ["vectors.f32", "queries.f32", "ids.i64", "truth.json"]},
    }
    (output / "manifest.json").write_bytes(_json_bytes(manifest))
    return manifest


def _validate_results(results, truth, k, allowed_ids=None):
    _require(isinstance(results, list) and len(results) == len(truth) and len(truth) > 0,
             "results must include every query in manifest order")
    for row in results:
        _require(isinstance(row, list) and len(row) == k, "each query requires exactly k results")
        _require(all(_integer(i) for i in row), "result IDs must be positive int64 integers")
        _require(len(set(row)) == k, "duplicate result ID")
        if allowed_ids is not None:
            _require(all(i in allowed_ids for i in row), "result ID is outside the dataset")


def recall_at_k(results, truth, k, allowed_ids=None):
    """Mean set intersection / K; rows are aligned to the manifest query order."""
    _require(_integer(k), "k must be positive")
    _validate_results(truth, truth, k, allowed_ids)
    _validate_results(results, truth, k, allowed_ids)
    return math.fsum(len(set(found) & set(expected)) / k
                     for found, expected in zip(results, truth)) / len(truth)


def _dataset(manifest_path):
    path = Path(manifest_path)
    manifest = _load_json(path)
    _require(isinstance(manifest, dict) and type(manifest.get("schema_version")) is int
             and manifest["schema_version"] == 1, "invalid manifest version")
    shape = manifest.get("shape")
    _require(isinstance(shape, dict) and all(_integer(shape.get(key)) for key in ["rows", "queries", "dimensions", "k"]),
             "invalid manifest shape")
    _require(shape["k"] <= shape["rows"] and manifest.get("metric") in METRICS,
             "invalid manifest metric or k")
    _require(manifest.get("tie_policy") == TIE_POLICY
             and manifest.get("vector_format") == "little_endian_float32_row_major"
             and manifest.get("id_format") == "little_endian_signed_int64", "unsupported manifest format")
    files = manifest.get("files")
    _require(isinstance(files, dict), "manifest files missing")
    sizes = {"vectors.f32": shape["rows"] * shape["dimensions"] * 4,
             "queries.f32": shape["queries"] * shape["dimensions"] * 4,
             "ids.i64": shape["rows"] * 8}
    for name in [*sizes, "truth.json"]:
        info = files.get(name)
        artifact = path.parent / name
        _require(isinstance(info, dict) and artifact.is_file(), f"missing dataset file: {name}")
        _require(artifact.stat().st_size == info.get("bytes") and sha256_file(artifact) == info.get("sha256"),
                 f"dataset hash/size mismatch: {name}")
        if name in sizes:
            _require(artifact.stat().st_size == sizes[name], f"dataset shape mismatch: {name}")
    ids = [value[0] for value in struct.iter_unpack("<q", (path.parent / "ids.i64").read_bytes())]
    _require(all(_integer(i) for i in ids) and len(set(ids)) == len(ids), "invalid dataset IDs")
    for name in ["vectors.f32", "queries.f32"]:
        with (path.parent / name).open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                _require(all(math.isfinite(v[0]) for v in struct.iter_unpack("<f", chunk)), "nonfinite dataset vector")
    truth = _load_json(path.parent / "truth.json")
    _require(isinstance(truth, dict) and isinstance(truth.get("ids"), list)
             and len(truth["ids"]) == shape["queries"], "invalid truth query count")
    _validate_results(truth["ids"], truth["ids"], shape["k"], set(ids))
    distances = truth.get("distances")
    _require(isinstance(distances, list) and len(distances) == shape["queries"], "truth distances missing")
    for row in distances:
        _require(isinstance(row, list) and len(row) == shape["k"]
                 and all(type(v) in (int, float) and math.isfinite(v) for v in row)
                 and row == sorted(row), "invalid truth distances")
    return manifest, truth["ids"], set(ids), sha256_file(path)


def _gpu_evidence(evidence, run_id, root):
    _require(isinstance(evidence, dict) and evidence.get("kind") == "profiler_kernel_trace",
             "GPU build requires structured profiler kernel evidence; linked libraries are insufficient")
    _require(evidence.get("build_run_id") == run_id and _text(evidence.get("device")),
             "GPU evidence must identify the device and this build run")
    kernels = evidence.get("kernels")
    _require(isinstance(kernels, list) and kernels, "GPU build kernel events missing")
    for kernel in kernels:
        _require(isinstance(kernel, dict) and _text(kernel.get("name"))
                 and _integer(kernel.get("launch_count")) and _positive(kernel.get("duration_ns"))
                 and kernel.get("phase") == "build", "invalid GPU build kernel event")
    _require(_text(evidence.get("artifact")), "GPU evidence artifact path missing")
    artifact = Path(evidence["artifact"])
    if not artifact.is_absolute():
        artifact = root / artifact
    _require(artifact.is_file(), f"GPU evidence artifact does not exist: {artifact}")
    _require(artifact.stat().st_size > 0 and sha256_file(artifact) == evidence.get("sha256"),
             "GPU evidence artifact hash mismatch or empty artifact")


def validate_experiment(records, manifest_path, *, evidence_root=None, targets=None):
    """Validate raw experiments and return a JSON-serializable report.

    A target assessment is solely numerical, conditional on later human review
    of the profiler and measurement protocol. Missing targets => unassessed.
    """
    _require(isinstance(records, dict) and type(records.get("schema_version")) is int
             and records["schema_version"] == 1, "invalid records version")
    _require(records.get("label") in ("MEASURED", "TEST ONLY"), "label must be MEASURED or TEST ONLY")
    # Also reject NaN hidden in configuration or other preserved raw fields.
    try:
        _json_bytes(records)
    except (TypeError, ValueError, OverflowError) as error:
        raise ValueError(f"records are not finite JSON: {error}") from error
    manifest, truth, allowed_ids, manifest_hash = _dataset(manifest_path)
    runs = records.get("runs")
    _require(isinstance(runs, list) and runs, "runs must be a nonempty list")
    root = Path(evidence_root) if evidence_root is not None else Path(manifest_path).parent
    times, recalls = {"cpu": [], "gpu": []}, {"cpu": [], "gpu": []}
    seen, reference = set(), None
    for run in runs:
        _require(isinstance(run, dict), "run must be an object")
        run_id, mode = run.get("run_id"), run.get("mode")
        _require(_text(run_id) and run_id not in seen and isinstance(mode, str) and mode in times,
                 "invalid/duplicate run ID or mode")
        seen.add(run_id)
        _require(run.get("manifest_sha256") == manifest_hash, "run dataset does not match the manifest")
        config, quota, versions = run.get("config"), run.get("cpu_quota"), run.get("source_versions")
        _require(isinstance(config, dict) and config.get("metric") == manifest["metric"]
                 and type(config.get("dimensions")) is int and config["dimensions"] == manifest["shape"]["dimensions"]
                 and type(config.get("k")) is int and config["k"] == manifest["shape"]["k"]
                 and isinstance(config.get("quality"), dict) and config["quality"], "invalid data/quality config")
        _require(isinstance(quota, dict) and _positive(quota.get("cores")) and _text(quota.get("affinity")),
                 "CPU quota requires positive cores and an affinity description")
        _require(isinstance(versions, dict) and _text(versions.get("seekdb")) and _text(versions.get("hipvs")),
                 "source_versions must identify seekdb and hipvs")
        comparable = _json_bytes({"config": config, "cpu_quota": quota, "source_versions": versions})
        if reference is None:
            reference = comparable
        _require(comparable == reference, "CPU/GPU data, quality config, quota or versions differ")
        _require(run.get("measurement") == "index_build_end_to_end", "only end-to-end index build time is accepted")
        _require(_positive(run.get("end_to_end_seconds")), "end-to-end seconds must be finite and positive")
        _require(run.get("fresh_index") is True and run.get("all_build_work_included") is True,
                 "every run must build a fresh index and include all build/deferred work")
        _require(run.get("cpu_fallback") is False and run.get("actual_backend") == ("cpu" if mode == "cpu" else "hipvs"),
                 "actual backend mismatch or CPU fallback")
        index = run.get("index")
        _require(isinstance(index, dict) and all(index.get(key) is True for key in
                 ["ready", "queryable", "complete", "query_plan_uses_index"])
                 and type(index.get("indexed_count")) is int and index["indexed_count"] == manifest["shape"]["rows"],
                 "index must be complete, ready, queryable, fully counted, and used by the SQL plan")
        if mode == "gpu":
            _gpu_evidence(run.get("gpu_evidence"), run_id, root)
        recalls[mode].append(recall_at_k(run.get("results"), truth, manifest["shape"]["k"], allowed_ids))
        times[mode].append(run["end_to_end_seconds"])
    _require(all(len(group) >= 3 for group in times.values()), "CPU and GPU each require at least three rounds")
    medians = {}
    for mode, group in times.items():
        ordered = sorted(group)
        middle = len(ordered) // 2
        medians[mode] = (ordered[middle] if len(ordered) % 2 else
                         ordered[middle - 1] + (ordered[middle] - ordered[middle - 1]) / 2)
    speedup = medians["cpu"] / medians["gpu"]
    _require(math.isfinite(speedup) and speedup > 0, "speedup is outside the finite representable range")
    assessment = "unassessed"
    if targets is not None:
        _require(isinstance(targets, dict), "targets must be an object")
        if all(key in targets for key in ["speedup_min", "recall_min", "source"]):
            _require(_positive(targets["speedup_min"]) and type(targets["recall_min"]) in (int, float)
                     and math.isfinite(targets["recall_min"]) and 0 <= targets["recall_min"] <= 1
                     and _text(targets["source"]), "invalid official targets or source")
            assessment = "pass" if (speedup >= targets["speedup_min"] and
                          min(recalls["cpu"] + recalls["gpu"]) >= targets["recall_min"]) else "fail"
    return {
        "schema_version": 1, "label": records["label"], "assessment": assessment,
        "assessment_scope": "numerical targets only; profiler/protocol require human review",
        "targets": targets, "manifest_sha256": manifest_hash, "metric": manifest["metric"],
        "tie_policy": TIE_POLICY, "raw_seconds": times, "median_seconds": medians,
        "speedup": speedup, "recall_at_k": recalls,
        "gpu_evidence_manual_review_required": True, "raw_runs": runs,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    prep = sub.add_parser("prepare", help="generate deterministic FP32 data and exact truth")
    prep.add_argument("--output", type=Path, required=True)
    for name, default in [("rows", 1000), ("queries", 20), ("dimensions", 32), ("k", 10), ("seed", 42)]:
        prep.add_argument(f"--{name}", type=int, default=default)
    prep.add_argument("--metric", choices=METRICS, default="l2_squared")
    for command in ["validate", "report"]:
        validator = sub.add_parser(command, help="validate raw build records and emit a report")
        validator.add_argument("--manifest", type=Path, required=True)
        validator.add_argument("--records", type=Path, required=True)
        validator.add_argument("--evidence-root", type=Path)
        validator.add_argument("--targets", type=Path, help="JSON with speedup_min, recall_min, source")
        validator.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "prepare":
            result = prepare_dataset(args.output, rows=args.rows, queries=args.queries,
                                     dimensions=args.dimensions, k=args.k, seed=args.seed, metric=args.metric)
        else:
            result = validate_experiment(_load_json(args.records), args.manifest,
                                        evidence_root=args.evidence_root or args.records.parent,
                                        targets=_load_json(args.targets) if args.targets else None)
        payload = _json_bytes(result)
        if args.command != "prepare" and args.output is not None:
            args.output.write_bytes(payload)
        else:
            sys.stdout.write(payload.decode("utf-8"))
        return 0
    except (ValueError, OSError, OverflowError, struct.error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
