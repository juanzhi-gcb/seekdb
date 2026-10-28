"""TEST ONLY: SQL runner contracts; these tests do not exercise a database."""
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

MODULE = Path(__file__).with_name("sql_smoke.py")
spec = importlib.util.spec_from_file_location("sql_smoke", MODULE)
smoke = importlib.util.module_from_spec(spec) if MODULE.exists() else None
if smoke is not None:
    sys.path.insert(0, str(MODULE.parent))
    spec.loader.exec_module(smoke)


class SqlSmokeTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(smoke, "sql_smoke.py has not been implemented")
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.output = Path(self.temp.name) / "run"

    def options(self, *extra):
        return smoke.parse_args(["--output", str(self.output), "--mode", "cpu", *extra])

    def test_owned_schema_is_unique_safe_and_does_not_accept_user_identifiers(self):
        first, second = smoke.new_schema(), smoke.new_schema()
        self.assertNotEqual(first, second)
        self.assertRegex(first, r"^hipvs_smoke_[0-9a-f]{32}$")
        self.assertLessEqual(len(first), 64)
        for value in ["a`;DROP DATABASE x", "a.b", "", "x-y"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                smoke.identifier(value)

    def test_sql_uses_fp32_roundtrippable_values_and_noncontiguous_original_ids(self):
        rounded = struct.unpack("<f", struct.pack("<f", 0.1))[0]
        sql = smoke.insert_sql("owned", [101, 409], [[rounded, -1.0], [0.0, 1.0]])
        self.assertIn("(101, '[", sql)
        self.assertIn("(409, '[", sql)
        self.assertIn(repr(rounded), sql)
        self.assertNotIn("CREATE VECTOR INDEX", smoke.table_sql("owned", 2))
        ddl = smoke.index_sql("owned", 64)
        for expected in ["CREATE VECTOR INDEX", "distance=l2", "type=hnsw", "lib=vsag",
                         "m=16", "ef_construction=200", "ef_search=64"]:
            self.assertIn(expected, ddl)
        query = smoke.query_sql("owned", [rounded, -1.0], 10)
        self.assertIn("l2_distance", query)
        self.assertIn("APPROXIMATE LIMIT 10", query)

    def test_sql_rejects_unsafe_shapes_ids_and_nonfinite_values(self):
        for ids, vectors in [([True], [[1.0]]), ([0], [[1.0]]), ([101, 101], [[1.0], [2.0]]),
                             ([101], [[float("nan")]]), ([101], [[float("inf")]]),
                             ([101, 409], [[1.0], [1.0, 2.0]]), ([101], [])]:
            with self.subTest(ids=ids, vectors=vectors), self.assertRaises(ValueError):
                smoke.insert_sql("owned", ids, vectors)
        with self.assertRaises(ValueError):
            smoke.query_sql("owned", [0.0], "1; DROP TABLE vectors")

    def test_results_preserve_full_int64_ids_and_require_exactly_k_distinct_known_ids(self):
        large = 9223372036854775789
        self.assertEqual(smoke.parse_ids("101\n{}\n".format(large), 2, {101, large}), [101, large])
        for output in ["101\n101\n", "101\n", "101\n409\n777\n", "101\n999\n",
                       "101\n409.0\n", "101\t409\n", "0\n409\n", "id\n101\n409\n"]:
            with self.subTest(output=output), self.assertRaises(ValueError):
                smoke.parse_ids(output, 2, {101, 409})

    def test_explain_requires_index_on_vector_scan_operator_not_only_predicate(self):
        valid = "|0|VECTOR INDEX SCAN|vectors(smoke_vec_idx)|10|\nOutputs: embedding"
        self.assertTrue(smoke.plan_uses_index(valid))
        self.assertTrue(smoke.plan_uses_index("|1|VECTOR INDEX PRE-FILTER SCAN|vectors(smoke_vec_idx)|"))
        for plan in ["TABLE FULL SCAN vectors; predicate smoke_vec_idx", "VECTOR INDEX SCAN vectors(other)",
                     "VECTOR INDEX SCAN vectors(other)\nOutputs: smoke_vec_idx",
                     "VECTOR INDEX SCAN vectors(smoke_vec_idx_other)"]:
            with self.subTest(plan=plan):
                self.assertFalse(smoke.plan_uses_index(plan))

    def test_invalid_cli_values_stop_before_any_database_action(self):
        for extra in [["--port", "0"], ["--host", "--execute=DROP"], ["--rows", "1"],
                      ["--k", "513"], ["--dimensions", "0"], ["--queries", "0"],
                      ["--ef-search", "0"], ["--timeout", "nan"], ["--user", "x\ny"]]:
            with self.subTest(extra=extra), mock.patch("sys.stderr", new_callable=io.StringIO), self.assertRaises((ValueError, SystemExit)):
                self.options(*extra)

    def test_mysql_uses_stdin_no_shell_or_password_argv_and_suppresses_credentials(self):
        options = self.options("--host", "db-host", "--user", "secret-user")
        client = smoke.MysqlClient(options, self.output)
        self.output.mkdir()
        completed = mock.Mock(returncode=0, stdout="101\n", stderr="")
        with mock.patch.object(smoke.subprocess, "run", return_value=completed) as run:
            output = client.execute("query", "SELECT id FROM vectors;", database="owned")
        self.assertEqual(output, "101\n")
        args, kwargs = run.call_args
        self.assertFalse(kwargs.get("shell", False))
        self.assertEqual(kwargs["input"], "SELECT id FROM vectors;")
        self.assertNotIn("-p", args[0])
        self.assertIn("--batch", args[0])
        self.assertIn("--skip-column-names", args[0])
        logged = "".join(path.read_text() for path in self.output.iterdir())
        self.assertNotIn("secret-user", logged)
        self.assertNotIn("db-host", logged)
        with mock.patch.object(smoke.subprocess, "run", return_value=mock.Mock(
                returncode=1, stdout="", stderr="Access denied secret-user secret-password")):
            with self.assertRaisesRegex(RuntimeError, "query_failed") as caught:
                client.execute("query_failed", "SELECT 1;")
        self.assertNotIn("secret-user", str(caught.exception))
        self.assertNotIn("secret-password", "".join(path.read_text() for path in self.output.iterdir()))

    def test_mysql_retains_safe_error_code_and_timeout_state_without_client_diagnostics(self):
        self.output.mkdir()
        client = smoke.MysqlClient(self.options(), self.output)
        completed = mock.Mock(returncode=1, stdout="", stderr="ERROR 1045 (28000): denied secret-password")
        with mock.patch.object(smoke.subprocess, "run", return_value=completed), self.assertRaises(RuntimeError):
            client.execute("create_schema", "CREATE DATABASE owned;")
        self.assertTrue((self.output / "create_schema.client.json").exists(), "safe mysql diagnostic metadata is missing")
        metadata = json.loads((self.output / "create_schema.client.json").read_text())
        self.assertEqual(metadata["mysql_error_code"], 1045)
        self.assertEqual(metadata["sqlstate"], "28000")
        self.assertEqual(metadata["status"], "failed")
        self.assertNotIn("secret-password", json.dumps(metadata))
        with mock.patch.object(smoke.subprocess, "run", side_effect=subprocess.TimeoutExpired("secret-password", 3)), self.assertRaises(RuntimeError):
            client.execute("create_index", "CREATE VECTOR INDEX idx ON owned(embedding);")
        metadata = json.loads((self.output / "create_index.client.json").read_text())
        self.assertEqual(metadata["status"], "timeout_server_outcome_unknown")
        self.assertNotIn("secret-password", json.dumps(metadata))

    def fake_client(self, fail_create=False):
        case = self
        class FakeClient:
            def __init__(self):
                self.calls = []
                self.timings = {}
            def execute(self, step, sql, database=None):
                self.calls.append((step, sql))
                self.timings[step] = 0.01
                if step == "create_schema" and fail_create:
                    raise RuntimeError("schema creation failed")
                if step == "base_count":
                    return "512\n"
                if step == "index_absent":
                    return "0\n"
                if step.startswith("explain_"):
                    return "|0|VECTOR INDEX SCAN|vectors(smoke_vec_idx)|10|"
                if step.startswith("query_"):
                    truth = json.loads((case.output / "data" / "truth.json").read_text())
                    return "".join(str(i) + "\n" for i in truth["ids"][int(step.split("_")[1])])
                return ""
        return FakeClient()

    def test_runner_inserts_before_ddl_preserves_data_and_reports_only_smoke(self):
        client = self.fake_client()
        report = smoke.run_smoke(self.options(), client=client)
        steps = [step for step, _ in client.calls]
        self.assertLess(steps.index("insert_0000"), steps.index("create_index"))
        self.assertLess(steps.index("index_absent"), steps.index("create_index"))
        self.assertFalse(any("DROP " in sql.upper() for _, sql in client.calls))
        self.assertEqual(report["measurement"], "smoke_only")
        self.assertEqual(report["recall_at_k"], 1.0)
        self.assertEqual(len(report["results"]), 32)
        self.assertEqual(report["base_table_count"], 512)
        self.assertIsNone(report["index"]["indexed_count"])
        self.assertEqual(report["index"]["complete"], "unverified")
        self.assertEqual(report["actual_backend"], "unverified")
        self.assertEqual(report["assessment"], "unassessed")
        self.assertIn("ddl_client_wall_seconds", report)
        self.assertIn("first_query_client_wall_seconds", report)
        self.assertTrue((self.output / "report.json").exists())

    def test_create_schema_failure_stops_without_drop_or_existing_schema_reuse(self):
        client = self.fake_client(fail_create=True)
        with self.assertRaises(RuntimeError):
            smoke.run_smoke(self.options(), client=client)
        self.assertEqual(len(client.calls), 1)
        self.assertNotIn("IF NOT EXISTS", client.calls[0][1])
        report = json.loads((self.output / "report.json").read_text())
        self.assertEqual(report["status"], "failed")
        self.assertFalse(report["schema_created"])

    def test_cpu_gpu_labels_use_identical_dataset_but_do_not_attest_backend(self):
        cpu = smoke.run_smoke(self.options(), client=self.fake_client())
        manifest = (self.output / "data" / "manifest.json").read_bytes()
        self.output = self.output.with_name("gpu")
        gpu = smoke.run_smoke(self.options("--mode", "gpu"), client=self.fake_client())
        self.assertEqual((self.output / "data" / "manifest.json").read_bytes(), manifest)
        self.assertEqual(cpu["manifest_sha256"], gpu["manifest_sha256"])
        self.assertEqual(cpu["config"], gpu["config"])
        self.assertEqual(gpu["actual_backend"], "unverified")
        self.assertEqual(gpu["assessment"], "unassessed")

    def test_cli_missing_mysql_fails_with_retained_schema_intent_and_no_claim(self):
        result = subprocess.run([sys.executable, str(MODULE), "--output", str(self.output),
                                 "--mode", "gpu", "--mysql", "definitely_missing_smoke_mysql_771a"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertNotIn("Traceback", result.stderr)
        report = json.loads((self.output / "report.json").read_text())
        self.assertEqual(report["status"], "failed")
        self.assertFalse(report["schema_created"])
        self.assertEqual(report["actual_backend"], "unverified")

    def test_existing_output_is_never_overwritten(self):
        self.output.mkdir()
        sentinel = self.output / "keep.txt"
        sentinel.write_text("keep")
        client = self.fake_client()
        with self.assertRaises(FileExistsError):
            smoke.run_smoke(self.options(), client=client)
        self.assertEqual(sentinel.read_text(), "keep")
        self.assertEqual(client.calls, [])

    def test_plan_mismatch_fails_and_preserves_failure_report(self):
        client = self.fake_client()
        execute = client.execute
        client.execute = lambda step, sql, database=None: (
            "TABLE FULL SCAN vectors" if step.startswith("explain_") else execute(step, sql, database))
        with self.assertRaisesRegex(ValueError, "plan"):
            smoke.run_smoke(self.options(), client=client)
        report = json.loads((self.output / "report.json").read_text())
        self.assertEqual(report["status"], "failed")
        self.assertTrue(report["schema_created"])
        self.assertNotIn("recall_at_k", report)


if __name__ == "__main__":
    unittest.main()
