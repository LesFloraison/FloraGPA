"""Preserve native texture UAV comparisons, failures and bounded execution evidence."""

import argparse
import itertools
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity
from summarize_dxbc_texture_loads import execution_report


def read_run(path):
    text = path.read_text(encoding="utf-8-sig")
    totals = re.search(r"Totals: (\d+) passed, (\d+) failed, (\d+) skipped.*?, (\d+)ms", text)
    unhandled = "Caught unhandled exception" in text
    result = {**identity(path), "completed": totals is not None and not unhandled,
              "totals_present": totals is not None, "unhandled_exception": unhandled,
              "passed": int(totals[1]) if totals else None,
              "failed": int(totals[2]) if totals else None,
              "skipped": int(totals[3]) if totals else None,
              "elapsed_ms": int(totals[4]) if totals else None,
              "failures": re.findall(r"^(?:FAIL!|QFATAL)\s*: (.*)$", text, re.MULTILINE),
              "probes": []}
    for line in text.splitlines():
        if not line.startswith("QINFO") or " {" not in line:
            continue
        probe = json.loads(line[line.index(" {") + 1:])
        if probe.pop("probe", None) != "dxbc-texture-uav":
            continue
        case = re.search(r"DxbcTextureUavTests::([^ ]+)", line).group(1)
        probe["test_passed"] = bool(re.search(
            r"^PASS\s+: DxbcTextureUavTests::" + re.escape(case) + r"$", text, re.MULTILINE))
        result["probes"].append(probe)
    result["unit_cases"] = re.findall(
        r"^PASS\s+: DxbcTextureUavTests::((?:validityAndTransactions|cancellationAndRecovery|cooperativeAndRollback)\([^)]*\))$",
        text, re.MULTILINE)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["log", "regression", "execution", "source", "build"]:
        parser.add_argument("--" + name, type=Path, action="append", default=[], required=name == "log")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    runs = [read_run(path) for path in args.log]
    regressions = [read_run(path) for path in args.regression]
    expected = {(vendor, dimension, fmt, mode)
                for vendor, dimension, fmt in itertools.product(
                    [0, 4318, 32902], [2, 3, 5, 7, 8], [41, 42, 43, 16, 28, 10, 24])
                for mode in range(8 if dimension == 5 else 7)}
    probes = runs[-1]["probes"]
    observed = {tuple(p[key] for key in ("vendor", "dimension", "format", "mode")) for p in probes}
    units = {"validityAndTransactions(" + name + ")" for name in [
        "packed-shared-bytes", "missing-default-components", "independent-rg-channels"]}
    units |= {"cancellationAndRecovery()"}
    units |= {f"cooperativeAndRollback({vendor})" for vendor in [0, 4318, 32902]}
    complete = observed == expected and len(probes) == len(expected)
    success = complete and set(runs[-1]["unit_cases"]) == units and all(
        p["passed"] and p["test_passed"] for p in probes) and all(
            r["completed"] and r["failed"] == 0 and r["skipped"] == 0 for r in [runs[-1], *regressions])
    for run in runs[:-1]:
        run["probe_count"] = len(run.pop("probes"))
    builds = []
    for path in args.build:
        text = path.read_text(encoding="utf-8-sig")
        builds.append({**identity(path), "compiler_errors": [
            line for line in text.splitlines() if re.search(r"error (?:C|LNK)\d+", line)]})
    report = {"schema": 1, "scope": "native_dxbc_texture_uav_research",
              "complete_release_acceptance": False, "native_reference_stage": "cs",
              "hardware_matrix_complete": complete, "latest_validation_passed": success,
              "sources": [identity(p) for p in args.source], "builds": builds,
              "runs": runs, "regressions": regressions,
              "executions": [execution_report(p) for p in args.execution]}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(runs)} runs; complete texture UAV validation: {success}")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
