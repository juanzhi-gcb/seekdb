"""Synthetic fixtures are TEST ONLY; no measured GPU performance is claimed."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

MODULE_PATH = Path(__file__).with_name("hipvs_eval.py")
spec = importlib.util.spec_from_file_location("hipvs_eval", MODULE_PATH)
ev = importlib.util.module_from_spec(spec) if MODULE_PATH.exists() else None
if ev is not None:
    spec.loader.exec_module(ev)


class EvaluationTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(ev, "hipvs_eval.py has not been implemented")
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        ev.prepare_dataset(self.root / "data", rows=9, queries=3, dimensions=4,
                           k=3, seed=42)
        self.manifest = self.root / "data" / "manifest.json"
        self.truth = json.loads((self.manifest.parent / "truth.json").read_text())
        self.artifact = self.root / "trace.json"
        self.artifact.write_text('{"fixture": "TEST ONLY", "events": []}')

    def records(self):
        runs = []
        for mode, times in [("cpu", [1, 2, 100]), ("gpu", [1, 4, 5])]:
            for i, seconds in enumerate(times):
                run_id = f"{mode}-{i}"
                run = {
                    "run_id": run_id, "mode": mode,
                    "manifest_sha256": ev.sha256_file(self.manifest),
                    "config": {"metric": "l2_squared", "dimensions": 4,
                               "k": 3, "quality": {"M": 16, "ef_search": 64}},
                    "cpu_quota": {"cores": 4, "affinity": "0-3"},
                    "source_versions": {"seekdb": "test-only-revision", "hipvs": "test-only-version"},
                    "measurement": "index_build_end_to_end",
                    "end_to_end_seconds": seconds,
                    "fresh_index": True, "all_build_work_included": True,
                    "actual_backend": "cpu" if mode == "cpu" else "hipvs",
                    "cpu_fallback": False,
                    "index": {"ready": True, "queryable": True, "complete": True,
                              "indexed_count": 9, "query_plan_uses_index": True},
                    "results": copy.deepcopy(self.truth["ids"]),
                }
                if mode == "gpu":
                    run["gpu_evidence"] = {
                        "kind": "profiler_kernel_trace", "artifact": "trace.json",
                        "sha256": ev.sha256_file(self.artifact),
                        "device": "TEST ONLY synthetic device", "build_run_id": run_id,
                        "kernels": [{"name": "TEST ONLY synthetic build kernel",
                                     "launch_count": 1, "duration_ns": 100, "phase": "build"}],
                    }
                runs.append(run)
        return {"schema_version": 1, "label": "TEST ONLY", "runs": runs}

    def validate(self, records=None, targets=None):
        return ev.validate_experiment(records or self.records(), self.manifest,
                                      evidence_root=self.root, targets=targets)

    def test_preparation_is_deterministic_fp32_and_noncontiguous_ids(self):
        other = self.root / "other"
        ev.prepare_dataset(other, rows=9, queries=3, dimensions=4, k=3, seed=42)
        for name in ["vectors.f32", "queries.f32", "ids.i64", "truth.json", "manifest.json"]:
            self.assertEqual((other / name).read_bytes(), (self.manifest.parent / name).read_bytes())
        vectors = (other / "vectors.f32").read_bytes()
        self.assertEqual(len(vectors), 9 * 4 * 4)
        ids = list(struct.unpack("<9q", (other / "ids.i64").read_bytes()))
        self.assertTrue(all(b - a > 1 for a, b in zip(ids, ids[1:])))
        manifest = json.loads(self.manifest.read_text())
        self.assertEqual(manifest["seed"], 42)
        self.assertEqual(manifest["metric"], "l2_squared")
        self.assertEqual(manifest["shape"], {"rows": 9, "queries": 3, "dimensions": 4, "k": 3})

    def test_recall_uses_document_ids(self):
        self.assertEqual(ev.recall_at_k([[101, 409]], [[101, 409]], 2), 1.0)
        self.assertEqual(ev.recall_at_k([[101, 777]], [[101, 409]], 2), 0.5)

    def test_exact_truth_ties_use_ascending_id(self):
        ids, distances = ev.exact_truth([[1.0], [-1.0], [3.0]], [[0.0]], [409, 101, 999], 2, "l2_squared")
        self.assertEqual(ids, [[101, 409]])
        self.assertEqual(distances, [[1.0, 1.0]])

    def test_medians_raw_times_and_missing_targets_unassessed(self):
        result = self.validate()
        self.assertEqual(result["speedup"], 0.5)
        self.assertEqual(result["raw_seconds"]["cpu"], [1, 2, 100])
        self.assertEqual(result["recall_at_k"]["gpu"], [1, 1, 1])
        self.assertEqual(result["assessment"], "unassessed")
        self.assertEqual(result["label"], "TEST ONLY")
        self.assertTrue(result["gpu_evidence_manual_review_required"])

    def test_explicit_targets_evaluate_all_run_recalls(self):
        targets = {"speedup_min": 0.4, "recall_min": 0.99, "source": "TEST ONLY"}
        self.assertEqual(self.validate(targets=targets)["assessment"], "pass")
        targets["speedup_min"] = 1.0
        self.assertEqual(self.validate(targets=targets)["assessment"], "fail")
        self.assertEqual(self.validate(targets={"speedup_min": 1})["assessment"], "unassessed")

    def test_invalid_results_are_rejected(self):
        for bad in [[[101, 101, 118]] * 3, [[-1, 118, 135]] * 3,
                    [[0, 1, 2]] * 3, [[101, 118]] * 3, [],
                    [[True, 118, 135]] * 3, [[101.0, 118, 135]] * 3]:
            with self.subTest(results=bad):
                records = self.records()
                records["runs"][0]["results"] = bad
                with self.assertRaises(ValueError):
                    self.validate(records)

    def test_not_enough_rounds_and_duplicate_run_ids_rejected(self):
        for change in [lambda r: r["runs"].pop(),
                       lambda r: r["runs"][0].update(run_id="cpu-1")]:
            records = self.records()
            change(records)
            with self.assertRaises(ValueError):
                self.validate(records)

    def test_nonfinite_nonpositive_or_query_timings_rejected(self):
        for bad in [0, -1, float("nan"), float("inf"), True, "3"]:
            records = self.records()
            records["runs"][0]["end_to_end_seconds"] = bad
            with self.subTest(seconds=bad), self.assertRaises(ValueError):
                self.validate(records)
        records = self.records()
        records["runs"][0]["measurement"] = "query_throughput"
        with self.assertRaises(ValueError):
            self.validate(records)

    def test_mismatched_data_quality_quota_versions_rejected(self):
        for key, value in [("manifest_sha256", "0" * 64), ("config", {"metric": "cosine"}),
                           ("cpu_quota", {"cores": 8}), ("source_versions", {"seekdb": "other"})]:
            records = self.records()
            records["runs"][-1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.validate(records)

    def test_incomplete_fallback_and_reused_builds_rejected(self):
        for key, value in [("ready", False), ("queryable", False), ("complete", False),
                           ("indexed_count", 8), ("query_plan_uses_index", False)]:
            records = self.records()
            records["runs"][-1]["index"][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.validate(records)
        for key, value in [("cpu_fallback", True), ("actual_backend", "cpu"),
                           ("fresh_index", False), ("all_build_work_included", False)]:
            records = self.records()
            records["runs"][-1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.validate(records)

    def test_gpu_evidence_requires_structure_and_verified_artifact(self):
        changes = [None, "linked libhipvs.so", {"kind": "linked_library"}]
        base = self.records()["runs"][-1]["gpu_evidence"]
        for key, value in [("artifact", "missing.trace"), ("sha256", "0" * 64),
                           ("build_run_id", "different-run"), ("kernels", []),
                           ("kernels", [{"name": "query", "launch_count": 1, "duration_ns": 1, "phase": "query"}])]:
            evidence = copy.deepcopy(base)
            evidence[key] = value
            changes.append(evidence)
        for evidence in changes:
            records = self.records()
            records["runs"][-1]["gpu_evidence"] = evidence
            with self.subTest(evidence=evidence), self.assertRaises(ValueError):
                self.validate(records)

    def test_tampered_dataset_rejected(self):
        with (self.manifest.parent / "vectors.f32").open("ab") as stream:
            stream.write(b"x")
        with self.assertRaises(ValueError):
            self.validate()

    def test_invalid_prepare_shape_rejected(self):
        for options in [{"rows": 0}, {"queries": 0}, {"dimensions": 0}, {"k": 10}, {"seed": True}]:
            args = dict(rows=9, queries=3, dimensions=4, k=3, seed=42)
            args.update(options)
            with self.subTest(options=options), self.assertRaises(ValueError):
                ev.prepare_dataset(self.root / "bad", **args)

    def test_cli_prepare_validate_report(self):
        result = subprocess.run([sys.executable, str(MODULE_PATH), "prepare", "--output", str(self.root / "cli"),
                                 "--rows", "4", "--queries", "2", "--dimensions", "2", "--k", "2"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        records_path = self.root / "records.json"
        records_path.write_text(json.dumps(self.records()))
        report_path = self.root / "report.json"
        for command in ["validate", "report"]:
            args = [sys.executable, str(MODULE_PATH), command, "--manifest", str(self.manifest),
                    "--records", str(records_path), "--output", str(report_path)]
            result = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(report_path.read_text())
            self.assertEqual(report["assessment"], "unassessed")
            self.assertEqual(report["label"], "TEST ONLY")
        records_path.write_text('{"runs": []}')
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)

    def test_malformed_modes_and_boolean_versions_fail_as_validation_errors(self):
        for mode in [[], {}, None, 1]:
            records = self.records()
            records["runs"][0]["mode"] = mode
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                self.validate(records)
        records = self.records()
        records["schema_version"] = True
        with self.assertRaises(ValueError):
            self.validate(records)

    def test_manifest_boolean_version_is_not_an_integer_version(self):
        manifest = json.loads(self.manifest.read_text())
        manifest["schema_version"] = True
        self.manifest.write_text(json.dumps(manifest))
        with self.assertRaises(ValueError):
            self.validate()

    def test_cli_rejects_duplicate_json_keys(self):
        records_path = self.root / "duplicate.json"
        records_path.write_text('{"schema_version": 1, "schema_version": 1}')
        result = subprocess.run([sys.executable, str(MODULE_PATH), "validate", "--manifest", str(self.manifest),
                                 "--records", str(records_path)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("duplicate JSON key", result.stderr)

    def test_supported_metric_distance_semantics(self):
        for metric, expected in [("l2", [1.0, 2.0]), ("l2_squared", [1.0, 4.0]),
                                  ("inner_product", [-6.0, -2.0]), ("cosine", [0.0, 0.0])]:
            vectors = [[1.0], [3.0]] if metric in ("inner_product", "cosine") else [[1.0], [4.0]]
            ids, distances = ev.exact_truth(vectors, [[2.0]], [101, 409], 2, metric)
            self.assertEqual(distances[0], expected)
        with self.assertRaises(ValueError):
            ev.exact_truth([[0.0]], [[1.0]], [101], 1, "cosine")

    def test_exact_truth_rejects_nonfinite_intermediates_and_l2_overflow(self):
        cases = [
            ("cosine", [[-1e200], [1e200]], [[1e200]]),
            ("cosine", [[-1e100], [1e100]], [[1e100]]),
            ("inner_product", [[1e200], [2e200]], [[1e200]]),
            ("l2", [[-1e200], [1e200]], [[1e200]]),
            ("l2_squared", [[-1e200], [1e200]], [[1e200]]),
            ("inner_product", [[10 ** 1000], [1]], [[1]]),
        ]
        for metric, vectors, queries in cases:
            with self.subTest(metric=metric, vectors=vectors), self.assertRaisesRegex(ValueError, "finite|overflow"):
                ev.exact_truth(vectors, queries, [101, 118], 1, metric)

    def test_even_count_large_finite_timings_have_finite_median(self):
        records = self.records()
        for run in records["runs"]:
            run["end_to_end_seconds"] = 1e308
        for index in [0, 3]:
            extra = copy.deepcopy(records["runs"][index])
            extra["run_id"] += "-extra"
            if extra["mode"] == "gpu":
                extra["gpu_evidence"]["build_run_id"] = extra["run_id"]
            records["runs"].append(extra)
        report = self.validate(records)
        self.assertEqual(report["median_seconds"], {"cpu": 1e308, "gpu": 1e308})
        self.assertEqual(report["speedup"], 1.0)

    def test_api_rejects_excessive_json_nesting_as_value_error(self):
        records = self.records()
        nested = 0
        for _ in range(1100):
            nested = [nested]
        records["nested"] = nested
        with self.assertRaisesRegex(ValueError, "nesting|depth"):
            self.validate(records)

    def test_cli_rejects_excessive_json_nesting_with_exit_two(self):
        records_path = self.root / "too-deep.json"
        records_path.write_text('{"nested":' + '[' * 1100 + '0' + ']' * 1100 + '}')
        result = subprocess.run([sys.executable, str(MODULE_PATH), "validate", "--manifest", str(self.manifest),
                                 "--records", str(records_path)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("nesting", result.stderr)
        self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
