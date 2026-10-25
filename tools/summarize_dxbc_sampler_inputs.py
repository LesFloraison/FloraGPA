"""Validate native sampler snapshots, recovery checks and input comparison evidence."""

import argparse
import itertools
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity
from summarize_dxbc_texture_uav import read_run


def validate_samplers(items):
    if len(items) != 16 or [item["slot"] for item in items] != list(range(16)):
        raise ValueError("Sampler observation must contain every native slot exactly once")
    for item in items:
        descriptor = item["descriptor"]
        if type(item["object"]) is not int or item["object"] < 0 or type(item["experimental"]) is not bool:
            raise ValueError("Invalid sampler provenance")
        if descriptor is None:
            if item["object"] or item["experimental"]:
                raise ValueError("Bound sampler observation has no descriptor")
        elif (len(descriptor) != 13 or any(type(v) is not int or not 0 <= v <= 4294967295 for v in descriptor)
              or not item["object"] and not item["experimental"]):
            raise ValueError("Invalid observed sampler descriptor")


def read(path):
    result = read_run(path)
    result.pop("probes")
    text = path.read_text(encoding="utf-8-sig")
    rows, comparisons = [], []
    for line in text.splitlines():
        if not line.startswith("QINFO") or " {" not in line:
            continue
        item = json.loads(line[line.index(" {") + 1:])
        if not item.get("sampler_inputs_verified") and not item.get("native_sampler_input_comparison"):
            continue
        case = re.search(r"DxbcInputSnapshotTests::([^ ]+)", line).group(1)
        item["test_passed"] = bool(re.search(r"^PASS\s+: DxbcInputSnapshotTests::" + re.escape(case) + r"$",
                                               text, re.MULTILINE))
        (rows if item.get("sampler_inputs_verified") else comparisons).append(item)
    result["sampler_matrix"] = rows
    result["native_comparisons"] = comparisons
    result["recovery_passed"] = "PASS   : DxbcInputSnapshotTests::samplerBudgetAndRetry()" in text
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["log", "regression", "observation", "source", "build", "binary"]:
        parser.add_argument("--" + name, type=Path, action="append", default=[], required=name == "log")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    runs = [read(path) for path in args.log]
    final = runs[-1]
    expected = set(itertools.product([0, 4318, 32902], [0, 4, 5], range(4)))
    rows = final["sampler_matrix"]
    complete = len(rows) == 36 and {(r["vendor"], r["stage"], r["mode"]) for r in rows} == expected
    matrix_passed = complete and all(r["test_passed"] and r["slots"] == 16
                                    and r["source"] == "independent-replay-resource-inputs/7"
                                    and r["deduplicated_identity_verified"]
                                    and r["selected_event_state_unchanged"] for r in rows)
    comparisons = final["native_comparisons"]
    comparisons_passed = len(comparisons) == 3 and {r["vendor"] for r in comparisons} == {0, 4318, 32902}
    for row in comparisons:
        validate_samplers(row["samplers"])
        samplers, comparison = row["samplers"], row["native_comparison"]
        comparisons_passed &= (row["passed"] and row["test_passed"] and comparison["passed"]
                               and comparison["samplers_checked"] == 16
                               and comparison["matching_native_inputs"]
                               and comparison["original_observed_resources_unchanged"]
                               and samplers[2]["object"] == 60 and samplers[3]["object"] == 61
                               and samplers[2]["descriptor"] == samplers[3]["descriptor"]
                               and samplers[15]["descriptor"] is None
                               and bool(comparison["resources"])
                               and all(r["equal"] and r["expected_sha256"] == r["native_sha256"]
                                       for r in comparison["resources"]))
    observations = []
    for path in args.observation:
        report = json.loads(path.read_text(encoding="utf-8-sig"))
        if (report["schema"] != 6 or report["source"] != "independent-replay-resource-inputs/7"
                or not report["observed"] or report.get("error") or report["shader_execution_performed"]):
            raise ValueError("Invalid resource input observation")
        validate_samplers(report["samplers"])
        observations.append({**identity(path), **{k: report[k] for k in [
            "capture", "event", "stage", "frame_sha256", "shader", "shader_sha256", "adapter_vendor",
            "adapter_device", "adapter_luid", "samplers", "source", "elapsed_ms"]}})
    regressions = [read_run(path) for path in args.regression]
    for run in regressions:
        run["probe_count"] = len(run.pop("probes"))
    success = (matrix_passed and comparisons_passed and final["recovery_passed"] and final["passed"] == 82
               and all(r["completed"] and not r["failed"] and not r["skipped"] for r in [final, *regressions]))
    result = {"schema": 1, "scope": "native_dxbc_sampler_inputs", "complete_release_acceptance": False,
              "latest_validation_passed": success, "sampler_matrix_complete": complete,
              "sampler_matrix_passed": matrix_passed, "native_comparisons_passed": comparisons_passed,
              "runs": runs, "regressions": regressions, "capture_observations": observations,
              "sources": [identity(path) for path in args.source],
              "builds": [identity(path) for path in args.build],
              "binaries": [identity(path) for path in args.binary]}
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Sampler input validation passed: {success}; {len(observations)} real-capture observations")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
