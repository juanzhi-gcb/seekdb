"""Small, real MySQL/seekdb SQL functional smoke; never a build benchmark.

Only creates a fresh owned schema, which is retained even on failure. Server
backend selection is external; --mode labels intent and does not prove GPU use.
Python standard library plus the mysql CLI and adjacent hipvs_eval.py suffice.
"""
import argparse
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import uuid

import hipvs_eval as evaluation

TABLE = "vectors"
INDEX = "smoke_vec_idx"


def identifier(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,63}", value):
        raise ValueError("unsafe SQL identifier")
    return "`" + value + "`"


def new_schema():
    return "hipvs_smoke_" + uuid.uuid4().hex


def positive_int(value, name, maximum=(1 << 63) - 1):
    if type(value) is not int or not 1 <= value <= maximum:
        raise ValueError(name + " must be a positive integer within supported bounds")
    return value


def vector_literal(vector):
    if not isinstance(vector, list) or not vector:
        raise ValueError("vector must be nonempty")
    if not all(type(v) in (float, int) and math.isfinite(v) for v in vector):
        raise ValueError("vector components must be finite numbers")
    # repr of the decoded FP32 value survives decimal -> FP32 SQL conversion.
    return "'[" + ",".join(repr(v) for v in vector) + "]'"


def qualified(schema):
    return identifier(schema) + "." + identifier(TABLE)


def table_sql(schema, dimensions):
    positive_int(dimensions, "dimensions", 4096)
    return "CREATE TABLE {} (id BIGINT PRIMARY KEY, embedding VECTOR({}));".format(
        qualified(schema), dimensions)


def insert_sql(schema, ids, vectors):
    if not ids or not vectors or len(ids) != len(vectors) or len(set(ids)) != len(ids):
        raise ValueError("IDs and vectors must have equal nonzero length and unique IDs")
    values = []
    dimensions = len(vectors[0])
    for doc_id, vector in zip(ids, vectors):
        positive_int(doc_id, "document ID")
        if len(vector) != dimensions:
            raise ValueError("vector dimensions must match")
        values.append("({}, {})".format(doc_id, vector_literal(vector)))
    return "INSERT INTO {} (id, embedding) VALUES\n{};".format(qualified(schema), ",\n".join(values))


def index_sql(schema, ef_search):
    positive_int(ef_search, "ef_search", 100000)
    return ("CREATE VECTOR INDEX {} ON {}(embedding) WITH "
            "(distance=l2, type=hnsw, lib=vsag, m=16, ef_construction=200, ef_search={});").format(
                identifier(INDEX), qualified(schema), ef_search)


def query_sql(schema, query, k):
    positive_int(k, "k")
    return ("SELECT id FROM {} ORDER BY l2_distance(embedding, {}) "
            "APPROXIMATE LIMIT {};").format(qualified(schema), vector_literal(query), k)


def configured_query(sql, ef_search):
    positive_int(ef_search, "ef_search", 100000)
    return "SET SESSION ob_hnsw_ef_search={}; ".format(ef_search) + sql


def parse_ids(output, k, allowed_ids):
    positive_int(k, "k")
    lines = output.splitlines()
    if len(lines) != k or not all(re.fullmatch(r"[0-9]+", line) for line in lines):
        raise ValueError("SQL result must contain exactly K integer ID rows")
    ids = [int(line) for line in lines]
    if len(set(ids)) != k or not all(0 < i < (1 << 63) and i in allowed_ids for i in ids):
        raise ValueError("SQL result IDs must be unique positive int64 IDs from the dataset")
    return ids


def plan_uses_index(output):
    # Match the target in the same pipe-delimited operator row, never elsewhere
    # in predicates/output expressions. Fail closed on an unfamiliar format.
    target = re.compile(r"\b" + re.escape(INDEX) + r"\b", re.IGNORECASE)
    for line in output.splitlines():
        cells = line.split("|")
        for operator, obj in zip(cells, cells[1:]):
            if re.search(r"\bVECTOR INDEX\b.*\bSCAN\b", operator, re.IGNORECASE) and target.search(obj):
                return True
    return False


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="new artifact directory (must not exist)")
    parser.add_argument("--mode", choices=("cpu", "gpu"), required=True,
                        help="expected server backend label only; configure server separately")
    parser.add_argument("--mysql", default="mysql", help="mysql client executable")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--rows", type=int, default=512)
    parser.add_argument("--dimensions", type=int, default=32)
    parser.add_argument("--queries", type=int, default=32)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--ef-search", type=int, default=64)
    parser.add_argument("--timeout", type=float, default=300,
                        help="per mysql invocation timeout seconds; timed-out DDL may continue on server")
    args = parser.parse_args(argv)
    for name, maximum in [("port", 65535), ("rows", 8192), ("dimensions", 4096),
                          ("queries", 256), ("ef_search", 100000)]:
        positive_int(getattr(args, name), name, maximum)
    if args.rows <= 128 or not 1 <= args.k <= args.rows:
        raise ValueError("smoke requires rows > 128 and 1 <= k <= rows")
    if args.rows * args.queries * args.dimensions > 100000000:
        raise ValueError("exhaustive smoke truth is limited to 100 million component distances")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        raise ValueError("timeout must be finite and positive")
    for name in ("host", "user", "mysql"):
        value = getattr(args, name)
        if not value or value.startswith("-") or any(ord(c) < 32 for c in value):
            raise ValueError(name + " must be nonempty text without controls or option prefix")
    return args


class MysqlClient:
    def __init__(self, options, output):
        self.options = options
        self.output = Path(output)
        self.timings = {}
        self.last_step = None

    def execute(self, step, sql, database=None):
        identifier(step)
        self.last_step = step
        options = self.options
        command = [options.mysql, "--no-defaults", "--protocol=TCP", "--batch", "--raw",
                   "--skip-column-names", "--default-character-set=utf8mb4",
                   "--host=" + options.host, "--port=" + str(options.port), "--user=" + options.user]
        if database is not None:
            identifier(database)
            command.append("--database=" + database)
        # Credentials may be provided through the mysql client's MYSQL_PWD
        # environment. Never put passwords in argv or persist client diagnostics,
        # which can echo credentials/connection details. SQL is generated only.
        (self.output / (step + ".sql")).write_text(sql, encoding="utf-8")
        started = time.monotonic()
        metadata = {"status": "invocation_failed"}
        try:
            completed = subprocess.run(command, input=sql, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, universal_newlines=True,
                                       encoding="utf-8", timeout=options.timeout, shell=False)
            metadata.update(status="completed" if completed.returncode == 0 else "failed",
                            exit_code=completed.returncode)
            error_code = re.search(r"\bERROR ([0-9]+)(?: \(([A-Z0-9]{5})\))?", completed.stderr)
            if error_code:
                metadata["mysql_error_code"] = int(error_code.group(1))
                metadata["sqlstate"] = error_code.group(2)
        except subprocess.TimeoutExpired:
            metadata["status"] = "timeout_server_outcome_unknown"
            raise RuntimeError("mysql timed out at " + step + "; server outcome unknown") from None
        except (OSError, subprocess.SubprocessError):
            raise RuntimeError("mysql invocation failed at " + step) from None
        finally:
            self.timings[step] = time.monotonic() - started
            metadata["client_wall_seconds"] = self.timings[step]
            (self.output / (step + ".client.json")).write_text(
                json.dumps(metadata, sort_keys=True) + "\n", encoding="utf-8")
        if completed.returncode != 0:
            raise RuntimeError("mysql failed at {} (exit {}; client diagnostics suppressed)".format(
                step, completed.returncode))
        (self.output / (step + ".stdout.txt")).write_text(completed.stdout, encoding="utf-8")
        return completed.stdout


def run_smoke(options, client=None):
    output = options.output
    # Never overwrite artifacts, recover/reuse a schema, or drop any schema.
    output.mkdir(parents=True, exist_ok=False)
    schema = new_schema()
    client = client if client is not None else MysqlClient(options, output)
    report = {
        "schema_version": 1, "measurement": "smoke_only", "expected_mode": options.mode,
        "actual_backend": "unverified", "assessment": "unassessed", "status": "started",
        "schema": schema, "schema_created": False, "schema_retained": True,
        "index": {"name": INDEX, "indexed_count": None, "complete": "unverified",
                  "query_plan_uses_index": False},
        "limitations": ["Not a three-round end-to-end performance measurement",
                        "Backend, fallback, profiler and GPU execution evidence are not collected",
                        "Base-table COUNT(*) does not establish complete indexed-vector count",
                        "No agreed recall threshold; report measured recall without a passing claim",
                        "Incremental mutations and restart persistence are not exercised"],
        "config": {"rows": options.rows, "dimensions": options.dimensions, "queries": options.queries,
                   "k": options.k, "seed": options.seed, "metric": "l2",
                   "m": 16, "ef_construction": 200, "ef_search": options.ef_search},
        "tie_policy": evaluation.TIE_POLICY,
    }

    def save():
        report["client_wall_seconds_by_step"] = dict(client.timings)
        report["last_sql_step"] = getattr(client, "last_step", None)
        (output / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True,
                                                       allow_nan=False) + "\n", encoding="utf-8")

    save()
    try:
        data = output / "data"
        evaluation.prepare_dataset(data, rows=options.rows, queries=options.queries,
                                   dimensions=options.dimensions, k=options.k, seed=options.seed, metric="l2")
        report["manifest_sha256"] = evaluation.sha256_file(data / "manifest.json")
        values = struct.unpack("<{}f".format(options.rows * options.dimensions),
                               (data / "vectors.f32").read_bytes())
        vectors = [list(values[i:i + options.dimensions]) for i in range(0, len(values), options.dimensions)]
        values = struct.unpack("<{}f".format(options.queries * options.dimensions),
                               (data / "queries.f32").read_bytes())
        queries = [list(values[i:i + options.dimensions]) for i in range(0, len(values), options.dimensions)]
        ids = list(struct.unpack("<{}q".format(options.rows), (data / "ids.i64").read_bytes()))
        truth = json.loads((data / "truth.json").read_text(encoding="utf-8"))["ids"]
        client.execute("create_schema", "CREATE DATABASE " + identifier(schema) + ";")
        report["schema_created"] = True
        save()
        client.execute("create_table", table_sql(schema, options.dimensions), database=schema)
        for number, start in enumerate(range(0, len(ids), 128)):
            client.execute("insert_{:04d}".format(number),
                           insert_sql(schema, ids[start:start + 128], vectors[start:start + 128]), database=schema)
        count = client.execute("base_count", "SELECT COUNT(*) FROM " + qualified(schema) + ";", database=schema)
        if count.strip() != str(options.rows):
            raise ValueError("base-table row count differs from prepared dataset")
        report["base_table_count"] = options.rows
        absent = client.execute("index_absent", (
            "SELECT COUNT(*) FROM information_schema.statistics WHERE table_schema='{}' "
            "AND table_name='{}' AND index_name='{}';").format(schema, TABLE, INDEX), database=schema)
        if absent.strip() != "0":
            raise ValueError("target index already exists or index absence is unverified")
        client.execute("create_index", index_sql(schema, options.ef_search), database=schema)
        report["ddl_client_wall_seconds"] = client.timings["create_index"]
        save()
        results = []
        for number, query in enumerate(queries):
            sql = query_sql(schema, query, options.k)
            plan = client.execute("explain_{:04d}".format(number), configured_query("EXPLAIN " + sql, options.ef_search), database=schema)
            if not plan_uses_index(plan):
                raise ValueError("query plan does not confirm target vector index at query " + str(number))
            step = "query_{:04d}".format(number)
            result = parse_ids(client.execute(step, configured_query(sql, options.ef_search), database=schema), options.k, set(ids))
            results.append(result)
            if number == 0:
                report["first_query_client_wall_seconds"] = client.timings[step]
                report["index"]["queryable"] = True
                save()
        report["index"]["query_plan_uses_index"] = True
        report["results"] = results
        report["recall_at_k"] = evaluation.recall_at_k(results, truth, options.k, set(ids))
        report["status"] = "functional_checks_completed"
        save()
        return report
    except Exception:
        report["status"] = "failed"
        # Do not persist arbitrary exception text: executable/connection errors
        # can contain argv, credentials or connection identifiers.
        save()
        raise


def main(argv=None):
    try:
        args = parse_args(argv)
        report = run_smoke(args)
        print(json.dumps({"measurement": report["measurement"], "status": report["status"],
                          "schema": report["schema"], "recall_at_k": report["recall_at_k"],
                          "assessment": report["assessment"]}, sort_keys=True))
        return 0
    except (ValueError, OSError, RuntimeError, struct.error):
        print("SQL smoke failed; inspect retained report/SQL artifacts. Client diagnostics are suppressed.",
              file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
