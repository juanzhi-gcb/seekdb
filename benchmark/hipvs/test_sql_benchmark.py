"""TEST ONLY: offline SQL measurement contracts, never measured GPU evidence."""
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

MODULE = Path(__file__).with_name("sql_benchmark.py")
sys.path.insert(0, str(MODULE.parent))
spec = importlib.util.spec_from_file_location("sql_benchmark", MODULE)
benchmark = importlib.util.module_from_spec(spec) if MODULE.exists() else None
if benchmark is not None:
    spec.loader.exec_module(benchmark)


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class SqlBenchmarkTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(benchmark, "sql_benchmark.py has not been implemented")
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / "run"
        self.log = self.root / "server.log"
        self.log.write_text("TEST ONLY: old log prefix\n", encoding="utf-8")
        self.clock = Clock()

    def options(self, *extra):
        return benchmark.parse_args(["--output", str(self.output), "--mode", "cpu",
                                     "--server-log", str(self.log), "--rows", "160",
                                     "--dimensions", "4", "--queries", "2", "--k", "2", *extra])

    def client(self, mode="cpu", log_override=None, fail=None, pending=False):
        case = self

        class Client:
            def __init__(self):
                self.calls = []
                self.timings = {}
                self.last_step = None

            def execute(self, step, sql, database=None):
                self.calls.append((step, sql, database, case.clock.now))
                self.last_step = step
                duration = 10.0
                if step.endswith("create_index"):
                    duration = 2.0
                    line = ("[HIPVS_BUILD] actual_backend=cpu, gpu_build_submissions=0, "
                            "reason=cpu_selected(indexed_count=160, index_type_=0)\n")
                    if mode == "gpu":
                        line = ("[HIPVS_BUILD] success(input_count=160, actual_backend=hipvs, "
                                "indexed_count=160, device=0, bridge_version=test-only, "
                                "bridge_ms=999999, gpu_build_submissions=1)\n")
                    with case.log.open("a", encoding="utf-8") as stream:
                        stream.write(line if log_override is None else log_override)
                elif "explain_ready" in step:
                    duration = 1.0
                elif step.endswith("first_query"):
                    duration = 3.0
                elif "quality_query" in step:
                    duration = 5.0
                case.clock.now += duration
                self.timings[step] = duration
                if fail and fail in step:
                    raise RuntimeError("mysql timed out; server outcome unknown")
                if step.endswith("base_count"):
                    return "160\n"
                if step.endswith("index_absent"):
                    return "0\n"
                if "explain_ready" in step:
                    if pending and step.endswith("0000"):
                        return "|0|TABLE FULL SCAN|vectors|"
                    return "|0|VECTOR INDEX SCAN|vectors(benchmark_vec_idx)|2|"
                if step.endswith("first_query") or "quality_query" in step:
                    truth = json.loads((case.output / "data" / "truth.json").read_text())["ids"]
                    number = 0 if step.endswith("first_query") else int(step.rsplit("_", 1)[1])
                    return "".join(str(value) + "\n" for value in truth[number])
                return ""

        return Client()

    def run_case(self, options=None, client=None):
        return benchmark.run_benchmark(options or self.options(), client=client or self.client(),
                                       clock=self.clock, sleep=self.clock.sleep)

    def test_defaults_and_invalid_settings_fail_before_sql(self):
        defaults = benchmark.parse_args(["--output", str(self.output), "--mode", "gpu",
                                         "--server-log", str(self.log)])
        self.assertEqual((defaults.rows, defaults.dimensions, defaults.queries, defaults.k,
                          defaults.seed, defaults.m, defaults.ef_construction,
                          defaults.ef_search, defaults.repetitions),
                         (10000, 128, 16, 10, 42, 16, 200, 128, 3))
        for extra in [("--repetitions", "2"), ("--timeout", "nan"), ("--ready-timeout", "0"),
                      ("--poll-interval", "0"), ("--m", "7"), ("--ef-search", "0"),
                      ("--host", "x\ny"), ("--metric", "cosine"), ("--k", "161")]:
            with self.subTest(extra=extra), mock.patch("sys.stderr", new_callable=io.StringIO), self.assertRaises((ValueError, SystemExit)):
                self.options(*extra)

    def test_each_query_connection_sets_requested_search_effort(self):
        client = self.client()
        self.run_case(self.options("--ef-search", "512"), client)
        relevant = [sql for step, sql, _, _ in client.calls
                    if "explain_ready" in step or step.endswith("first_query") or "quality_query" in step]
        self.assertTrue(relevant)
        self.assertTrue(all(sql.startswith("SET SESSION ob_hnsw_ef_search=512; ") for sql in relevant))

    def test_three_fresh_builds_timer_covers_ready_and_first_query_only(self):
        client = self.client(pending=True)
        report = self.run_case(client=client)
        self.assertEqual(report["status"], "performance_observation_only")
        self.assertEqual(report["acceptance"], "acceptance_unassessed")
        # DDL 2 + first EXPLAIN 1 + poll 1 + second EXPLAIN 1 + first query 3.
        self.assertEqual(report["raw_seconds"], [8.0, 8.0, 8.0])
        self.assertEqual(len(report["runs"]), 3)
        creates = [call for call in client.calls if call[0].endswith("create_index")]
        drops = [call for call in client.calls if call[0].endswith("drop_index")]
        self.assertEqual(len(creates), 3)
        self.assertEqual(len(drops), 2)
        self.assertTrue(all(report["schema"] in call[1] for call in drops))
        self.assertFalse(any("DROP DATABASE" in sql for _, sql, _, _ in client.calls))
        for run, create in zip(report["runs"], creates):
            self.assertEqual(run["timer_started_monotonic"], create[3])
            self.assertEqual(run["recall_at_k"], 1.0)
            self.assertEqual(run["index"]["indexed_count"], 160)
            self.assertEqual(run["actual_backend"], "cpu")
            self.assertTrue((self.output / run["build_log"]["artifact"]).exists())
            self.assertEqual(run["build_log"]["correlation"], "isolated_single_build_window")
        first_create = client.calls.index(creates[0])
        self.assertTrue(any(step.startswith("insert_") for step, _, _, _ in client.calls[:first_create]))
        self.assertIn("generation_seconds", report)
        self.assertIn("raw_import_seconds", report)
        self.assertIsNone(report["server_startup_seconds"])

    def test_cpu_gpu_same_input_config_and_internal_ms_never_becomes_seconds(self):
        cpu = self.run_case()
        cpu_manifest = (self.output / "data" / "manifest.json").read_bytes()
        self.output = self.root / "gpu"
        gpu = self.run_case(self.options("--mode", "gpu"), self.client(mode="gpu"))
        self.assertEqual(cpu_manifest, (self.output / "data" / "manifest.json").read_bytes())
        self.assertEqual(cpu["config"], gpu["config"])
        self.assertEqual(cpu["manifest_sha256"], gpu["manifest_sha256"])
        self.assertEqual(gpu["raw_seconds"], [6.0, 6.0, 6.0])
        self.assertTrue(all(run["actual_backend"] == "hipvs" for run in gpu["runs"]))
        self.assertTrue(all(run["gpu_evidence"] is None for run in gpu["runs"]))
        self.assertFalse(gpu["evaluator_validated"])

    def test_each_insert_preserves_original_fp32_and_batches_at_most_128(self):
        client = self.client()
        self.run_case(client=client)
        inserts = [sql for step, sql, _, _ in client.calls if step.startswith("insert_")]
        self.assertEqual(len(inserts), 2)
        self.assertEqual(inserts[0].count("'[") , 128)
        self.assertIn("(101, '[", inserts[0])
        self.assertIn("(118, '[", inserts[0])

    def test_external_dataset_reuses_hash_checked_manifest_without_generation(self):
        self.run_case()
        dataset = self.output / "data" / "manifest.json"
        original = dataset.read_bytes()
        self.output = self.root / "second"
        report = self.run_case(self.options("--dataset", str(dataset)))
        self.assertEqual(original, (self.output / "data" / "manifest.json").read_bytes())
        self.assertEqual(report["generation_seconds"], 0.0)

    def test_selected_nondefault_manifest_is_not_replaced_by_sibling_manifest(self):
        data = self.root / "source_data"
        benchmark.evaluation.prepare_dataset(data, rows=160, dimensions=4, queries=2,
                                             k=2, seed=42, metric="l2_squared")
        selected = data / "chosen-manifest.json"
        selected.write_bytes((data / "manifest.json").read_bytes())
        selected_hash = benchmark.evaluation.sha256_file(selected)
        sibling = json.loads(selected.read_text(encoding="utf-8"))
        # TEST ONLY: both metadata variants pass the existing manifest validator;
        # the chosen file, not a differently named sibling, defines identity.
        sibling.update(metric="l2", seed=987)
        (data / "manifest.json").write_text(json.dumps(sibling), encoding="utf-8")
        benchmark.evaluation._dataset(selected)
        benchmark.evaluation._dataset(data / "manifest.json")
        self.assertNotEqual(selected_hash, benchmark.evaluation.sha256_file(data / "manifest.json"))
        reports = []
        for mode in ("cpu", "gpu"):
            self.output = self.root / ("selected_" + mode)
            report = self.run_case(self.options("--mode", mode, "--dataset", str(selected)),
                                   self.client(mode=mode))
            self.assertEqual((self.output / "data" / "manifest.json").read_bytes(),
                             selected.read_bytes())
            self.assertEqual(report["manifest_sha256"], selected_hash)
            self.assertEqual(report["config"]["metric"], "l2_squared")
            self.assertEqual(report["config"]["seed"], 42)
            self.assertTrue(all(run["manifest_sha256"] == selected_hash for run in report["runs"]))
            reports.append(report)
        self.assertEqual(reports[0]["config"], reports[1]["config"])

    def test_bad_backend_count_failure_and_ambiguous_logs_stop_after_first_round(self):
        lines = ["[HIPVS_BUILD] actual_backend=cpu, gpu_build_submissions=0, reason=cpu_selected(indexed_count=159)\n",
                 "[HIPVS_BUILD] success(actual_backend=hipvs, indexed_count=160, input_count=160, gpu_build_submissions=1)\n",
                 "[HIPVS_BUILD] explicit failure; no CPU fallback(ret=-1)\n",
                 "[HIPVS_BUILD] actual_backend=cpu, gpu_build_submissions=0, reason=no_eligible_ddl_batch(indexed_count=160)\n",
                 "unrelated indexed_count=160\n", ""]
        lines.append(lines[0].replace("159", "160") * 2)
        for number, line in enumerate(lines):
            self.output = self.root / ("bad" + str(number))
            client = self.client(log_override=line)
            with self.subTest(line=line), self.assertRaises((ValueError, RuntimeError)):
                self.run_case(client=client)
            report = json.loads((self.output / "report.json").read_text())
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["raw_seconds"], [])
            self.assertEqual(len([s for s, _, _, _ in client.calls if s.endswith("create_index")]), 1)

    def test_gpu_cpu_fallback_never_scores(self):
        with self.assertRaises(ValueError):
            self.run_case(self.options("--mode", "gpu"), self.client(mode="cpu"))
        report = json.loads((self.output / "report.json").read_text())
        self.assertEqual(report["raw_seconds"], [])

    def test_log_window_excludes_old_records_and_detects_rotation_and_truncation(self):
        self.log.write_text("[HIPVS_BUILD] actual_backend=cpu indexed_count=160\n", encoding="utf-8")
        cursor = benchmark.LogWindow(self.log)
        with self.log.open("ab") as stream:
            stream.write(b"new record\n")
        self.assertEqual(cursor.read(), b"new record\n")
        for action in ("truncate", "rotate", "rewrite_regrow"):
            self.log.write_bytes(b"prefix" * 100)
            cursor = benchmark.LogWindow(self.log)
            if action == "rotate":
                self.log.rename(self.root / "old.log")
                self.log.write_bytes(b"replacement" * 100)
            else:
                self.log.write_bytes(b"x" if action == "truncate" else b"replacement" * 100)
            with self.subTest(action=action), self.assertRaises(ValueError):
                cursor.read()

    def test_partial_second_build_record_cannot_hide_error_or_duplicate(self):
        success = (b"[HIPVS_BUILD] success(input_count=160, actual_backend=hipvs, "
                   b"indexed_count=160, gpu_build_submissions=1)\n")
        for suffix in (b"[HIPVS_BUILD] explicit failure", b"[HIPVS_BUILD] success("):
            with self.subTest(suffix=suffix):
                self.assertIsNone(benchmark.parse_build_log(success + suffix, "gpu", 160),
                                  "partial new build records must remain unverified")

    def test_quality_failure_or_late_duplicate_never_admits_round_time(self):
        for behavior in ("invalid_quality_ids", "late_duplicate"):
            self.output = self.root / behavior
            client = self.client()
            execute = client.execute

            def execute_bad(step, sql, database=None):
                result = execute(step, sql, database)
                if "quality_query_0000" in step:
                    if behavior == "invalid_quality_ids":
                        return "101\n101\n"
                    with self.log.open("a", encoding="utf-8") as stream:
                        stream.write("[HIPVS_BUILD] actual_backend=cpu, gpu_build_submissions=0, "
                                     "reason=cpu_selected(indexed_count=160)\n")
                return result

            client.execute = execute_bad
            with self.subTest(behavior=behavior), self.assertRaises(ValueError):
                self.run_case(client=client)
            report = json.loads((self.output / "report.json").read_text())
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["raw_seconds"], [])
            self.assertEqual(report["runs"][0]["end_to_end_seconds"], 6.0)
            self.assertFalse(any("DROP" in sql for _, sql, _, _ in client.calls))

    def test_timeout_stops_without_drop_or_next_build_and_retains_state(self):
        client = self.client(fail="create_index")
        with self.assertRaises(RuntimeError):
            self.run_case(client=client)
        report = json.loads((self.output / "report.json").read_text())
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["runs"][0]["status"], "failed_server_outcome_may_be_unknown")
        self.assertTrue(report["schema_retained"])
        self.assertEqual(report["raw_seconds"], [])
        self.assertFalse(any("DROP" in sql for _, sql, _, _ in client.calls))
        self.assertTrue((self.output / "round_0001.server.log").exists())

    def test_ready_timeout_and_invalid_first_result_fail_closed(self):
        for behavior in ("plan", "ids"):
            self.output = self.root / behavior
            client = self.client()
            execute = client.execute

            def execute_bad(step, sql, database=None):
                result = execute(step, sql, database)
                if behavior == "plan" and "explain_ready" in step:
                    return "|0|TABLE FULL SCAN|vectors|"
                if behavior == "ids" and step.endswith("first_query"):
                    return "101\n101\n"
                return result

            client.execute = execute_bad
            with self.assertRaises((ValueError, RuntimeError)):
                self.run_case(self.options("--ready-timeout", "2"), client)
            report = json.loads((self.output / "report.json").read_text())
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["raw_seconds"], [])

    def test_existing_output_and_unacknowledged_schema_never_reused_or_dropped(self):
        self.output.mkdir()
        client = self.client()
        with self.assertRaises(FileExistsError):
            self.run_case(client=client)
        self.assertEqual(client.calls, [])
        self.output = self.root / "schema_fail"
        client = self.client(fail="create_schema")
        with self.assertRaises(RuntimeError):
            self.run_case(client=client)
        self.assertEqual(len(client.calls), 1)
        self.assertNotIn("IF NOT EXISTS", client.calls[0][1])
        report = json.loads((self.output / "report.json").read_text())
        self.assertFalse(report["schema_created"])


if __name__ == "__main__":
    unittest.main()
