#!/usr/bin/env python3
"""Check complete native samples and retain raw evidence; never estimate official E."""
import json
import math
from pathlib import Path
import statistics
import sys

VARIANTS = ("baseline", "range", "write", "store", "combined")
METRICS = ("write_ms", "query_unfiltered_ms", "query_filtered_ms", "query_total_ms",
           "write_cpu_ms", "query_cpu_ms", "database_bytes", "insert_file_growth_bytes",
           "process_maxrss_kib", "local_cgroup_peak_bytes")


def read(path):
    with path.open() as stream:
        return json.load(stream)


def summarize(root):
    micro = {}
    for schema in ("bigint", "varchar"):
        samples = {variant: [read(root / "micro" / f"r{r}-{schema}-{variant}.json")
                             for r in (1, 2, 3)] for variant in VARIANTS}
        expected_digest = samples["baseline"][0]["result_digest"]
        for variant, rows in samples.items():
            for row in rows:
                assert row["rows"] == row["recovered_rows"] == 10000, (schema, variant)
                assert row["queries"] == row["validated_queries"] == 1000, (schema, variant)
                assert row["result_digest"] == expected_digest, (schema, variant)
                assert row["profile_enabled"] is False, "timed runs must not enable profiling"
                assert all(math.isfinite(row[k]) and row[k] > 0 for k in METRICS), (schema, variant)
        baseline = {k: statistics.median(r[k] for r in samples["baseline"]) for k in METRICS}
        micro[schema] = {}
        for variant, rows in samples.items():
            median = {k: statistics.median(r[k] for r in rows) for k in METRICS}
            micro[schema][variant] = {
                "median": median,
                "candidate_over_baseline": {k: median[k] / baseline[k] for k in METRICS},
                "samples": rows,
                "exact_results_and_restart_agree": True,
            }
    assert len(list((root / "micro").glob("r*-*.json"))) == 30
    contract = {}
    for count in (10000, 30000):
        for schema in ("bigint", "varchar"):
            path = root / "contract" / f"{count}-{schema}" / "summary.json"
            if not path.exists():
                assert count == 30000, path
                continue
            document = read(path)
            assert document["metadata"]["rows"] == count
            assert document["metadata"]["filter_type"] == schema
            expected_rounds = 3 if count == 10000 else 1
            assert document["metadata"]["rounds"] == expected_rounds
            assert len(document["groups"]) == 6
            for group in document["groups"].values():
                assert len(group["samples"]) == expected_rounds
                for sample in group["samples"]:
                    assert sample["passed"] is True
                    assert sample["swap_bytes"] == 0
                    assert all(math.isfinite(sample[k]) and sample[k] > 0
                               for k in ("T_ms", "B_bytes", "M_bytes"))
            ratios = {}
            for phase in ("write/unindexed", "query/unindexed", "query/indexed"):
                reference = document["groups"]["reference/" + phase]["median"]
                candidate = document["groups"]["candidate/" + phase]["median"]
                ratios[phase] = {k: candidate[k] / reference[k] for k in ("T_ms", "B_bytes", "M_bytes")}
            contract[f"{count}-{schema}"] = {
                "metadata": document["metadata"], "groups": document["groups"],
                "candidate_over_baseline": ratios,
            }
    isa = {variant: read(root / variant / "range-isa.json") for variant in ("range", "combined")}
    for result in isa.values():
        for name in ("avx2", "avx512"):
            assert result[name + "_range"] == ("passed" if result[name + "_supported"] else "unsupported")
    summary = {
        "scope": "same-host native diagnostics; not an official E or Prompt evaluation",
        "identity": read(root / "refs.json"), "isa_checks": isa,
        "micro": micro, "jsonl_contract": contract,
        "ratio_direction": "candidate / baseline; lower is better for all reported metrics",
        "limitations": ["Shared runners have variable CPUs and load; compare within each replica first.",
                        "Microbench RSS/file growth are diagnostics, not official M/B.",
                        "JSONL B/M retain startup/loading/warmup and full drain under the existing collector.",
                        "Unsupported AVX512 hardware leaves that execution path unverified on this replica."],
    }
    (root / "comparison.json").write_text(json.dumps(summary, indent=2, allow_nan=False) + "\n")
    print(json.dumps({"checked_micro_samples": 30, "checked_contracts": list(contract),
                      "isa_checks": isa, "summary": str(root / "comparison.json")}, allow_nan=False))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: summarize_native_candidates.py RESULT_DIRECTORY")
    summarize(Path(sys.argv[1]).resolve(strict=True))
