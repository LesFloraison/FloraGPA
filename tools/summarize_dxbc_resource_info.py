"""Record native resource-query comparisons without claiming shader parity."""

import argparse
import itertools
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity
from summarize_dxbc_texture_inputs import read_run as base_run
from summarize_dxbc_texture_loads import execution_report


def read_run(path):
    run = base_run(path)
    text = path.read_text(encoding="utf-8-sig")
    run["probes"] = []
    run["independent_hlsl"] = []
    for line in text.splitlines():
        if not line.startswith("QINFO") or " {" not in line:
            continue
        probe = json.loads(line[line.index(" {") + 1:])
        kind = probe.pop("probe", None)
        if kind not in ["dxbc-resource-info", "native-hlsl-resource-query"]:
            continue
        case = re.search(r"DxbcResourceInfoTests::([^ ]+)", line).group(1)
        probe["case"] = case
        probe["test_passed"] = bool(re.search(
            r"^PASS\s+: DxbcResourceInfoTests::" + re.escape(case) + r"$", text, re.MULTILINE))
        run["probes" if kind == "dxbc-resource-info" else "independent_hlsl"].append(probe)
    return run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, action="append", required=True)
    parser.add_argument("--regression", type=Path, action="append", default=[])
    parser.add_argument("--execution", type=Path, action="append", default=[])
    parser.add_argument("--source", type=Path, action="append", default=[])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    runs = [read_run(path) for path in args.log]
    regressions = [base_run(path) for path in args.regression]
    for run in regressions:
        run["probe_count"] = len(run.pop("probes"))
    expected = {
        (vendor, dimension, uav, mode, scenario)
        for vendor, dimension, uav, mode, scenario in itertools.product(
            [0, 4318, 32902], range(2, 11), [False, True], range(3), range(5))
        if not (uav and (dimension in [4, 6] or dimension >= 9))
        and not (scenario == 4 and dimension in [4, 9])}
    observed = {tuple(probe[key] for key in ("vendor", "dimension", "uav", "mode", "scenario"))
                for probe in runs[-1]["probes"]}
    complete = observed == expected and len(runs[-1]["probes"]) == len(expected)
    final_text = args.log[-1].read_text(encoding="utf-8-sig")
    unit_checks = {name: bool(re.search(r"^PASS\s+: DxbcResourceInfoTests::" + name +
                                      r"\(\)$", final_text, re.MULTILINE))
                   for name in ["validityAndSelection", "missingBindingsValidateOperands",
                                "gpuQueryCacheAndCancellation"]}
    extended = re.findall(r"^PASS\s+: DxbcResourceInfoTests::extendedDeclarations\(([^)]+)\)$",
                          final_text, re.MULTILINE)
    extended_complete = (len(extended) == 6 and set(extended) ==
                         {f"{vendor}-{uav}" for vendor in [0, 4318, 32902] for uav in [0, 1]})
    independent = runs[-1]["independent_hlsl"]
    independent_complete = (len(independent) == 6 and
                            {(p["vendor"], p["volume"]) for p in independent} ==
                            set(itertools.product([0, 4318, 32902], [False, True])) and
                            all(p["passed"] and p["test_passed"] for p in independent))
    success = complete and independent_complete and extended_complete and all(unit_checks.values()) and not runs[-1]["failed"] and not runs[-1]["skipped"] and all(
        probe["passed"] and probe["test_passed"] for probe in runs[-1]["probes"]) and all(
        not run["failed"] and not run["skipped"] for run in regressions)
    for run in runs[:-1]:
        probes = run.pop("probes")
        run["probe_count"] = len(probes)
        run["probe_failures"] = [probe for probe in probes if not probe["test_passed"]]
    report = {"schema": 1, "scope": "native_dxbc_resource_info_research",
              "complete_release_acceptance": False, "native_reference_stage": "cs",
              "hardware_matrix_complete": complete, "latest_validation_passed": success,
              "unit_checks": unit_checks,
              "independent_hlsl_complete": independent_complete,
              "extended_declarations_complete": extended_complete,
              "sources": [identity(path) for path in args.source], "runs": runs,
              "regressions": regressions, "executions": [execution_report(path) for path in args.execution]}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(runs)} runs; complete resource query validation: {success}")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
