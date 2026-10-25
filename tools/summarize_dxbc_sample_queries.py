"""Retain native sample-query execution results, regressions and independent evidence."""

import argparse
import itertools
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity
from summarize_dxbc_texture_uav import read_run


def read(path):
    result = read_run(path)
    text = path.read_text(encoding="utf-8-sig")
    rows, comparisons = [], []
    for line in text.splitlines():
        if not line.startswith("QINFO") or " {" not in line:
            continue
        item = json.loads(line[line.index(" {") + 1:])
        if item.get("native_query_comparison"):
            case = re.search(r"DxbcSampleQueryTests::([^ ]+)", line).group(1)
            item["test_passed"] = bool(re.search(r"^PASS\s+: DxbcSampleQueryTests::" +
                                                  re.escape(case) + r"$", text, re.MULTILINE))
            comparisons.append(item)
            continue
        if "vendor" not in item or "quality" not in item:
            continue
        case = re.search(r"DxbcSampleQueryTests::([^ ]+)", line).group(1)
        item["test_passed"] = bool(re.search(r"^PASS\s+: DxbcSampleQueryTests::" +
                                              re.escape(case) + r"$", text, re.MULTILINE))
        rows.append(item)
    result["hardware"] = rows
    result["native_comparisons"] = comparisons
    result["unit_cases"] = re.findall(
        r"^PASS\s+: DxbcSampleQueryTests::((?:unknownAndInvalid|stateRecovery)\([^)]*\))$",
        text, re.MULTILINE)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["log", "regression", "source", "build"]:
        parser.add_argument("--" + name, type=Path, action="append", default=[], required=name == "log")
    parser.add_argument("--prior-regression", type=Path, action="append", default=[])
    parser.add_argument("--evidence", type=Path, action="append", default=[])
    parser.add_argument("--binary", type=Path, action="append", default=[])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    runs = [read(path) for path in args.log]
    final = runs[-1]
    expected = set(itertools.product([0, 4318, 32902], [1, 2, 4, 8],
                                     [0, 4294967295, 4294967294], range(5), [False, True]))
    rows = final["hardware"]
    complete = len(rows) == len(expected) and {
        tuple(row[k] for k in ["vendor", "count", "quality", "mode", "multisample"])
        for row in rows} == expected
    regressions = [read_run(path) for path in args.regression]
    units = {"unknownAndInvalid()"} | {
        f"stateRecovery({vendor}-{mode})" for vendor, mode in itertools.product([0, 4318, 32902], [0, 2])}
    classified = all(r.get("semantics_verified") == (r["mode"] != 3)
                     and r.get("unknown_preserved") == (r["mode"] == 3) for r in rows)
    comparisons = final["native_comparisons"]
    comparisons_passed = (len(comparisons) == 6 and {(r["vendor"], r["mode"]) for r in comparisons}
                          == set(itertools.product([0, 4318, 32902], [0, 1])))
    for row in comparisons:
        comparison = row["native_comparison"]
        comparisons_passed &= (row["passed"] and row["test_passed"] and len(row["sample_queries"]) == 1
                               and row["sample_queries"][0]["observation"]["observed"]
                               and row["sample_queries"][0]["observation"]["count"] == 4
                               and len(row["sample_queries"][0]["observation"]["positions"]) == 4
                               and comparison["passed"]
                               and comparison["matching_native_inputs"]
                               and comparison["original_observed_resources_unchanged"]
                               and comparison["sample_queries_checked"] == 1
                               and len(comparison["resources"]) == 1
                               and all(r["equal"] and r["bytes"] == 64
                                       and r["expected_sha256"] == r["native_sha256"]
                                       for r in comparison["resources"]))
    success = (complete and classified and comparisons_passed and set(final["unit_cases"]) == units
               and final["passed"] == 375
               and all(r["passed"] and r["test_passed"] for r in rows) and all(
                   r["completed"] and not r["failed"] and not r["skipped"] for r in [final, *regressions]))
    for run in runs[:-1]:
        run["hardware_count"] = len(run.pop("hardware"))
        run["native_comparison_count"] = len(run.pop("native_comparisons"))
    for run in regressions:
        run["probe_count"] = len(run.pop("probes"))
    report = {"schema": 1, "scope": "native_dxbc_sample_queries", "complete_release_acceptance": False,
              "hardware_matrix_complete": complete, "latest_validation_passed": success,
              "sample_query_semantics_complete": False,
              "native_comparisons_passed": comparisons_passed,
              "verified_query_cases": sum(r.get("semantics_verified", False) for r in rows),
              "unresolved_no_target_cases": sum(r.get("unknown_preserved", False) for r in rows),
              "runs": runs, "regressions": regressions,
              "historical_regressions": [read_run(path) for path in args.prior_regression],
              "independent_evidence": [identity(path) for path in args.evidence],
              "binaries": [identity(path) for path in args.binary],
              "sources": [identity(path) for path in args.source],
              "builds": [identity(path) for path in args.build]}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(runs)} runs; sample-query checkpoint passed: {success}")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
