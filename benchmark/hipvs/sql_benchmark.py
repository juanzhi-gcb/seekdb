"""Collect real SQL end-to-end build observations against an existing server.

No server startup, GPU profiler collection, evaluator validation or acceptance
claim is performed. Only a fresh owned schema is modified, and it is retained.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import struct
import sys
import time
import uuid

import hipvs_eval as evaluation
import sql_smoke as smoke

INDEX = "benchmark_vec_idx"
DATA_FILES = ("manifest.json", "vectors.f32", "queries.f32", "ids.i64", "truth.json")


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="new artifact directory")
    parser.add_argument("--mode", choices=("cpu", "gpu"), required=True)
    parser.add_argument("--server-log", type=Path, required=True, help="local log from this isolated server")
    parser.add_argument("--dataset", type=Path, help="existing evaluator manifest; its shape/seed/metric take precedence")
    parser.add_argument("--mysql", default="mysql")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    for name, default in [("rows", 10000), ("dimensions", 128), ("queries", 16),
                          ("k", 10), ("seed", 42), ("m", 16), ("ef-construction", 200),
                          ("ef-search", 128), ("repetitions", 3)]:
        parser.add_argument("--" + name, type=int, default=default)
    parser.add_argument("--metric", choices=("l2", "l2_squared"), default="l2_squared")
    parser.add_argument("--timeout", type=float, default=1800, help="per mysql invocation timeout seconds")
    parser.add_argument("--ready-timeout", type=float, default=300,
                        help="total post-DDL plan/query/log readiness deadline seconds")
    parser.add_argument("--poll-interval", type=float, default=1.0)
    args = parser.parse_args(argv)
    for name, maximum in [("port", 65535), ("rows", (1 << 63) - 1), ("dimensions", 4096),
                          ("queries", (1 << 63) - 1), ("k", (1 << 63) - 1), ("m", 64),
                          ("ef_construction", 100000), ("ef_search", 100000), ("repetitions", 10000)]:
        smoke.positive_int(getattr(args, name), name, maximum)
    if args.m < 8 or args.repetitions < 3:
        raise ValueError("M must be in [8, 64] and repetitions must be at least three")
    if args.dataset is None:
        if args.rows <= max(128, 4 * args.m) or args.k > args.rows:
            raise ValueError("require rows > max(128, 4*M) and k <= rows")
        if args.rows * args.dimensions * args.queries > 100000000:
            raise ValueError("generated exhaustive truth exceeds 100 million component distances; use --dataset")
    for name in ("timeout", "ready_timeout", "poll_interval"):
        if not math.isfinite(getattr(args, name)) or getattr(args, name) <= 0:
            raise ValueError(name + " must be finite and positive")
    for name in ("host", "user", "mysql"):
        value = getattr(args, name)
        if not value or value.startswith("-") or any(ord(c) < 32 for c in value):
            raise ValueError(name + " must be nonempty text without controls or option prefix")
    if not args.server_log.is_file():
        raise ValueError("server log must exist before any SQL action")
    return args


class LogWindow:
    """Append-only window with file identity and old-prefix-tail integrity checks.

    Copy/truncate/rewrite is unsupported. inode changes, shortened files and
    rewritten anchors fail closed. The operator must disable log rotation for
    this experiment; an identical-byte rewrite cannot be detected by readers.
    """
    def __init__(self, path):
        self.path = Path(path)
        with self.path.open("rb") as stream:
            stat = os.fstat(stream.fileno())
            self.identity = (stat.st_dev, stat.st_ino)
            self.offset = stat.st_size
            self.anchor_start = max(0, self.offset - 4096)
            stream.seek(self.anchor_start)
            self.anchor = stream.read(self.offset - self.anchor_start)

    def read(self):
        with self.path.open("rb") as stream:
            stat = os.fstat(stream.fileno())
            if (stat.st_dev, stat.st_ino) != self.identity or stat.st_size < self.offset:
                raise ValueError("server log rotated or truncated; build window invalid")
            stream.seek(self.anchor_start)
            if stream.read(len(self.anchor)) != self.anchor:
                raise ValueError("server log prefix rewritten; build window invalid")
            stream.seek(self.offset)
            raw = stream.read(stat.st_size - self.offset)
            path_stat = self.path.stat()
            if ((path_stat.st_dev, path_stat.st_ino) != self.identity
                    or path_stat.st_size < stat.st_size or len(raw) != stat.st_size - self.offset):
                raise ValueError("server log changed identity or truncated during capture")
            return raw


def _field(line, name):
    # OceanBase LOG_INFO renders named fields as key=value; tolerate key: value
    # and quoted scalar values while preserving the raw record for verification.
    matches = re.findall(r"\b" + re.escape(name) + r"\s*[:=]\s*[\"']?([A-Za-z0-9_.+-]+)", line)
    if len(matches) != 1:
        raise ValueError("missing or ambiguous build log field: " + name)
    return matches[0]


def parse_build_log(raw, mode, rows):
    """None means no complete finalize record yet; any conflict is fatal."""
    text = raw.decode("utf-8", errors="replace")
    # A writer's partial last line must never become a prematurely valid record.
    if text and not text.endswith(("\n", "\r")) and "[HIPVS_BUILD]" in text.splitlines()[-1]:
        return None
    lines = [line for line in text.splitlines(keepends=True)
             if line.endswith(("\n", "\r")) and "[HIPVS_BUILD]" in line]
    for line in lines:
        if re.search(r"\b(failed|failure|exception|skipped_empty|no_eligible_ddl_batch|unsupported)\b", line, re.I):
            raise ValueError("build log reports failure, unsupported path, or CPU fallback")
    if not lines:
        return None
    if len(lines) != 1:
        raise ValueError("build window must contain exactly one finalize record")
    line = lines[0]
    backend = _field(line, "actual_backend")
    if backend != ("cpu" if mode == "cpu" else "hipvs"):
        raise ValueError("actual backend differs from requested mode; CPU fallback is not a score")
    count, submissions = _field(line, "indexed_count"), _field(line, "gpu_build_submissions")
    if not count.isdigit() or int(count) != rows or not submissions.isdigit():
        raise ValueError("actual GetNumElements indexed count differs from dataset")
    if mode == "cpu":
        if int(submissions) != 0 or _field(line, "reason") != "cpu_selected":
            raise ValueError("CPU finalize did not establish the ordinary CPU path")
    else:
        if not re.search(r"\[HIPVS_BUILD\]\s+success\b", line) or int(submissions) <= 0:
            raise ValueError("GPU finalize success and actual submission count required")
        if _field(line, "input_count") != str(rows):
            raise ValueError("GPU input count differs from dataset")
    return {"actual_backend": backend, "indexed_count": int(count),
            "gpu_build_submissions": int(submissions), "matched_raw_record": line.rstrip("\r\n")}


def index_sql(schema, options):
    return ("CREATE VECTOR INDEX {} ON {}(embedding) WITH "
            "(distance=l2, type=hnsw, lib=vsag, m={}, ef_construction={}, ef_search={});").format(
                smoke.identifier(INDEX), smoke.qualified(schema), options.m,
                options.ef_construction, options.ef_search)


def plan_uses_index(output):
    target = re.compile(r"\b" + re.escape(INDEX) + r"\b", re.I)
    for line in output.splitlines():
        cells = line.split("|")
        for operator, obj in zip(cells, cells[1:]):
            if re.search(r"\bVECTOR INDEX\b.*\bSCAN\b", operator, re.I) and target.search(obj):
                return True
    return False


def _read_matrix(path, dimensions):
    row_size = dimensions * 4
    with path.open("rb") as stream:
        while True:
            row = stream.read(row_size)
            if not row:
                return
            yield list(struct.unpack("<{}f".format(dimensions), row))


def run_benchmark(options, client=None, clock=time.monotonic, sleep=time.sleep):
    output = options.output
    output.mkdir(parents=True, exist_ok=False)
    schema = "hipvs_benchmark_" + uuid.uuid4().hex
    client = client if client is not None else smoke.MysqlClient(options, output)
    report = {
        "schema_version": 1, "measurement": "index_build_end_to_end",
        "status": "started", "expected_mode": options.mode,
        "acceptance": "acceptance_unassessed", "assessment": "unassessed",
        "evaluator_validated": False, "schema": schema, "schema_created": False,
        "schema_retained": True, "server_startup_seconds": None,
        "server_startup": "external_not_measured", "runs": [], "raw_seconds": [],
        "isolation_required": "Only this runner performs vector DDL in this server process/log; log rotation disabled",
        "log_correlation": "isolated_single_build_window; server records do not carry schema/index/run identity",
        "limitations": ["No bound SQL-run profiler artifact; logs alone do not prove GPU kernels",
                        "No official thresholds supplied; acceptance remains unassessed",
                        "Run CPU/GPU on the same source and resource allocation; alternate externally",
                        "No server startup, resource/memory measurement or restart regression is performed"],
    }
    current, window = None, None

    def save():
        report["client_wall_seconds_by_step"] = dict(client.timings)
        report["last_sql_step"] = getattr(client, "last_step", None)
        (output / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True,
                                                      allow_nan=False) + "\n", encoding="utf-8")

    def capture():
        raw = window.read()
        (output / current["build_log"]["artifact"]).write_bytes(raw)
        current["build_log"]["bytes"] = len(raw)
        return raw

    save()
    try:
        started = clock()
        data = output / "data"
        if options.dataset is None:
            evaluation.prepare_dataset(data, rows=options.rows, queries=options.queries,
                                       dimensions=options.dimensions, k=options.k,
                                       seed=options.seed, metric=options.metric)
            report["generation_seconds"] = clock() - started
        else:
            evaluation._dataset(options.dataset)
            data.mkdir()
            shutil.copyfile(options.dataset, data / "manifest.json")
            for name in DATA_FILES:
                if name != "manifest.json":
                    shutil.copyfile(options.dataset.parent / name, data / name)
            report["generation_seconds"] = 0.0
        manifest, truth, allowed, manifest_hash = evaluation._dataset(data / "manifest.json")
        shape = manifest["shape"]
        rows, dimensions, k = shape["rows"], shape["dimensions"], shape["k"]
        if (manifest["metric"] not in ("l2", "l2_squared") or dimensions > 4096
                or rows <= max(128, 4 * options.m)):
            raise ValueError("dataset not supported by this L2 SQL benchmark")
        ids = [value[0] for value in struct.iter_unpack("<q", (data / "ids.i64").read_bytes())]
        queries = list(_read_matrix(data / "queries.f32", dimensions))
        report["dataset_preparation_seconds"] = clock() - started
        report["manifest_sha256"] = manifest_hash
        report["config"] = dict(shape, seed=manifest.get("seed"), metric=manifest["metric"],
                                sql_distance="l2", m=options.m,
                                ef_construction=options.ef_construction, ef_search=options.ef_search)
        report["tie_policy"] = evaluation.TIE_POLICY
        client.execute("create_schema", "CREATE DATABASE " + smoke.identifier(schema) + ";")
        report["schema_created"] = True
        save()
        started = clock()
        client.execute("create_table", smoke.table_sql(schema, dimensions), database=schema)
        vectors = _read_matrix(data / "vectors.f32", dimensions)
        for number, offset in enumerate(range(0, rows, 128)):
            batch_ids = ids[offset:offset + 128]
            batch = [next(vectors) for _ in batch_ids]
            client.execute("insert_{:06d}".format(number), smoke.insert_sql(schema, batch_ids, batch), database=schema)
        count = client.execute("base_count", "SELECT COUNT(*) FROM " + smoke.qualified(schema) + ";", database=schema)
        if count.strip() != str(rows):
            raise ValueError("raw import row count differs from dataset")
        report["raw_import_seconds"] = clock() - started
        report["base_table_count_not_index_evidence"] = rows
        save()
        for repetition in range(1, options.repetitions + 1):
            prefix = "round_{:04d}".format(repetition)
            current = {"run_id": prefix + "_" + uuid.uuid4().hex, "mode": options.mode,
                       "status": "preparing", "fresh_index": False, "gpu_evidence": None,
                       "manifest_sha256": manifest_hash,
                       "index": {"name": INDEX, "indexed_count": None, "complete": False},
                       "build_log": {"artifact": prefix + ".server.log", "correlation": "isolated_single_build_window"}}
            report["runs"].append(current)
            window = None
            save()
            if repetition > 1:
                # This index was acknowledged as created by this invocation in
                # its UUID schema. No existing user schema/index is accepted.
                client.execute(prefix + "_drop_index", "DROP INDEX {} ON {};".format(
                    smoke.identifier(INDEX), smoke.qualified(schema)), database=schema)
            absent = client.execute(prefix + "_index_absent", (
                "SELECT COUNT(*) FROM information_schema.statistics WHERE table_schema='{}' "
                "AND table_name='{}' AND index_name='{}';").format(schema, smoke.TABLE, INDEX), database=schema)
            if absent.strip() != "0":
                raise ValueError("fresh target index absence is unverified")
            window = LogWindow(options.server_log)
            current["build_log"].update(start_offset=window.offset, file_device=window.identity[0],
                                         file_inode=window.identity[1])
            current["fresh_index"] = True
            current["status"] = "timing"
            save()
            current["timer_started_monotonic"] = clock()
            client.execute(prefix + "_create_index", index_sql(schema, options), database=schema)
            current["ddl_acknowledged"] = True
            deadline = clock() + options.ready_timeout

            def bounded_execute(step, sql):
                remaining = deadline - clock()
                if remaining <= 0:
                    raise RuntimeError("post-DDL readiness timeout; server outcome may be unknown")
                old_timeout = options.timeout
                try:
                    options.timeout = min(old_timeout, remaining)
                    return client.execute(step, sql, database=schema)
                finally:
                    options.timeout = old_timeout

            query = smoke.query_sql(schema, queries[0], k)
            attempt = 0
            while True:
                plan = bounded_execute(prefix + "_explain_ready_{:04d}".format(attempt), smoke.configured_query("EXPLAIN " + query, options.ef_search))
                if plan_uses_index(plan):
                    break
                remaining = deadline - clock()
                if remaining <= 0:
                    raise RuntimeError("target index plan readiness timeout")
                sleep(min(options.poll_interval, remaining))
                attempt += 1
            first = bounded_execute(prefix + "_first_query", smoke.configured_query(query, options.ef_search))
            smoke.parse_ids(first, k, allowed)
            while True:
                raw = capture()
                build = parse_build_log(raw, options.mode, rows)
                if build is not None:
                    break
                remaining = deadline - clock()
                if remaining <= 0:
                    raise RuntimeError("finalize indexed-count log readiness timeout")
                sleep(min(options.poll_interval, remaining))
            stopped = clock()
            elapsed = stopped - current["timer_started_monotonic"]
            if not math.isfinite(elapsed) or elapsed <= 0:
                raise ValueError("end-to-end time must be finite and positive")
            current.update(timer_stopped_monotonic=stopped, end_to_end_seconds=elapsed,
                           actual_backend=build["actual_backend"], cpu_fallback=False,
                           all_build_work_included=True, status="quality_checks")
            current["build_log"].update(build)
            current["index"].update(indexed_count=build["indexed_count"], complete=True,
                                     ready=True, queryable=True, query_plan_uses_index=True,
                                     count_source="finalize_GetNumElements_server_log")
            save()
            # Re-run the full quality workload outside the timer, including Q0.
            results = []
            for number, vector in enumerate(queries):
                result = client.execute(prefix + "_quality_query_{:04d}".format(number),
                                        smoke.configured_query(smoke.query_sql(schema, vector, k), options.ef_search), database=schema)
                results.append(smoke.parse_ids(result, k, allowed))
            # Catch delayed extra finalize/error records before admitting a score.
            final_build = parse_build_log(capture(), options.mode, rows)
            if final_build != build:
                raise ValueError("build log changed after first query")
            current.update(results=results, recall_at_k=evaluation.recall_at_k(results, truth, k, allowed),
                           status="performance_observation_only")
            current["build_log"]["sha256"] = evaluation.sha256_file(output / current["build_log"]["artifact"])
            report["raw_seconds"].append(elapsed)
            save()
        report["median_seconds"] = statistics.median(report["raw_seconds"])
        report["status"] = "performance_observation_only"
        save()
        return report
    except Exception as error:
        report["status"] = "failed"
        report["failure_kind"] = type(error).__name__
        if current is not None:
            current["status"] = "failed_server_outcome_may_be_unknown"
            if window is not None:
                try:
                    capture()
                except (OSError, ValueError):
                    current["build_log"]["capture_status"] = "invalid_or_unavailable_log_window"
        save()
        raise


def main(argv=None):
    try:
        report = run_benchmark(parse_args(argv))
        print(json.dumps({key: report[key] for key in
                          ("status", "acceptance", "schema", "raw_seconds", "median_seconds")}, sort_keys=True))
        return 0
    except (ValueError, OSError, RuntimeError, struct.error, OverflowError):
        print("SQL benchmark failed; inspect retained report, SQL and log artifacts. Client diagnostics suppressed.",
              file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
