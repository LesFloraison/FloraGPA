"""Accept the complete comparison sampling execution matrix and adjacent regressions."""

import argparse
import itertools
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity
from summarize_dxbc_texture_uav import read_run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", default="dxbc-gpu/14")
    for name in ("log", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("regression", "source", "build", "binary", "evidence"):
        parser.add_argument("--" + name, type=Path, action="append", default=[])
    args = parser.parse_args()
    text = args.log.read_text(encoding="utf-8-sig")
    rows = []
    for line in text.splitlines():
        if line.startswith("QINFO") and " {" in line:
            row = json.loads(line[line.index(" {") + 1:])
            if row.get("sample_comparison_verified"):
                rows.append(row)
    expected = set(itertools.product((0, 4318, 32902), range(56)))
    if len(rows) != 168 or {(r["vendor"], r["configuration"]) for r in rows} != expected:
        raise ValueError("Incomplete comparison execution matrix")
    for row in rows:
        undefined = row["configuration"] in (52, 53)
        if (row["comparisons"] != (0 if undefined else 189)
                or row["undefined_rejections"] != (3 if undefined else 0)
                or row["complete_group_verified"] != (not undefined)
                or row["helper"]["backend"] != args.backend):
            raise ValueError("Comparison execution weakened defined or undefined-result assertions")
    units = {f"{name}({v}-{s})" for name, v, s in itertools.product(
        ("modifiersAndMasks", "cancellation"), (0, 4318, 32902), (0, 1, 5))}
    units |= {"unknownAndInvalid()", "requestValidation()"}
    passed_units = set(re.findall(r"^PASS\s+: DxbcSampleComparisonTests::((?:modifiersAndMasks|cancellation|unknownAndInvalid|requestValidation)\([^)]*\))$", text, re.MULTILINE))
    if passed_units != units:
        raise ValueError("Incomplete comparison validity, modifier or cancellation checks")
    final = read_run(args.log)
    regressions = [read_run(p) for p in args.regression]
    regression_keys = []
    for path, run in zip(args.regression, regressions):
        name = re.search(r"Start testing of (\w+)", path.read_text(encoding="utf-8-sig"))
        if not name:
            raise ValueError("Missing regression suite identity")
        regression_keys.append((name.group(1), run["passed"]))
    expected_regressions = {("DxbcSampleLevelTests", 262), ("DxbcSampleLevelTests", 156),
                            ("DxbcSamplingGroupTests", 296), ("DxbcGroupTests", 38),
                            ("DxbcExecutionTests", 375)}
    if len(regression_keys) != 5 or set(regression_keys) != expected_regressions:
        raise ValueError("Required comparison sampling regressions are missing")
    if final["passed"] != 190 or any(not r["completed"] or r["failed"] or r["skipped"]
                                      for r in (final, *regressions)):
        raise ValueError("Comparison sampling or its required regressions failed")
    for run in (final, *regressions):
        run.pop("probes")
    result = {"schema": 1, "scope": "native_dxbc_sample_comparison_zero", "validation_passed": True,
              "complete_release_acceptance": False, "final": final, "regressions": regressions,
              "hardware_cases": len(rows), "per_stage_vectors": sum(r["comparisons"] for r in rows),
              "complete_compute_groups": sum(r["complete_group_verified"] for r in rows),
              "undefined_sampler_rejections": sum(r["undefined_rejections"] for r in rows),
              "unit_cases": sorted(units), "sources": [identity(p) for p in args.source],
              "builds": [identity(p) for p in args.build], "binaries": [identity(p) for p in args.binary],
              "supporting_evidence": [identity(p) for p in args.evidence]}
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Validated 168 comparison configurations, 20 semantic checks and five regression suites")


if __name__ == "__main__":
    main()
