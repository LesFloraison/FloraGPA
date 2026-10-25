"""Validate explicit-LOD execution matrices without hiding earlier failures."""

import argparse
import itertools
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity
from summarize_dxbc_texture_uav import read_run


def read(path, gradient):
    result = read_run(path)
    result.pop("probes")
    text = path.read_text(encoding="utf-8-sig")
    hardware, shapes, batches = [], [], []
    for line in text.splitlines():
        if not line.startswith("QINFO") or " {" not in line:
            continue
        item = json.loads(line[line.index(" {") + 1:])
        if not (item.get("sample_grad_verified" if gradient else "sample_level_verified")
                or item.get("sample_shape_verified") or item.get("native_sampling_batch_verified")):
            continue
        case = re.search(r"DxbcSampleLevelTests::([^ ]+)", line).group(1)
        item["test_passed"] = bool(re.search(r"^PASS\s+: DxbcSampleLevelTests::" + re.escape(case) + r"$",
                                               text, re.MULTILINE))
        (batches if item.get("native_sampling_batch_verified")
         else shapes if item.get("sample_shape_verified") else hardware).append(item)
    result["hardware"] = hardware
    result["shapes"] = shapes
    result["batches"] = batches
    result["unit_cases"] = re.findall(
        r"^PASS\s+: DxbcSampleLevelTests::((?:cancellation|unknownAndInvalid|batchCancellation|batchValidation)\([^)]*\))$", text, re.MULTILINE)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["log", "regression", "source", "build", "binary", "evidence"]:
        parser.add_argument("--" + name, type=Path, action="append", default=[], required=name == "log")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--grad", action="store_true")
    parser.add_argument("--backend", default="dxbc-gpu/12")
    parser.add_argument("--execution-model", default="bounded-sampling-rendezvous/3")
    args = parser.parse_args()
    runs = [read(path, args.grad) for path in args.log]
    final = runs[-1]
    expected = set(itertools.product([0, 4318, 32902], range(34)))
    hardware = final["hardware"]
    hardware_complete = len(hardware) == len(expected) and {
        (r["vendor"], r["configuration"]) for r in hardware} == expected
    shapes = final["shapes"]
    expected_shapes = set(itertools.product([0, 4318, 32902], [2, 3, 5, 6, 7, 8, 10], [2, 28]))
    shapes_complete = len(shapes) == len(expected_shapes) and {
        (r["vendor"], r["dimension"], r["format"]) for r in shapes} == expected_shapes
    units = {"unknownAndInvalid()"} | {
        f"cancellation({vendor}-{stage})" for vendor, stage in itertools.product([0, 4318, 32902], [0, 1, 5])}
    batches = final["batches"]
    batch_complete = (len(batches) == len(expected) and
                      {(r["vendor"], r["configuration"]) for r in batches} == expected)
    if args.grad:
        units |= {"batchValidation()"} | {f"batchCancellation({v})" for v in [0, 4318, 32902]}
    regressions = [read_run(path) for path in args.regression]
    verified = (hardware_complete and shapes_complete and set(final["unit_cases"]) == units
                and final["passed"] == (262 if args.grad else 156)
                and (not args.grad or batch_complete and all(
                    r["test_passed"] and r["records"] == 63 and r["active_lanes"] == 63
                    and r["group_size"] == [64, 1, 1] and r["helper"]["backend"] == args.backend for r in batches))
                and all(r["completed"] and not r["failed"] and not r["skipped"] for r in [final, *regressions])
                and all(r["test_passed"] and r["group_output_verified"] and r["records"] == 63
                        and r.get("lane_output_verified", True)
                        and (not args.grad or (r.get("reference_stages") == ["ps", "vs", "cs"]
                            and r.get("lane_reference_invocations") == "single"
                            and r.get("group_reference_invocations") == "batch"
                            and r.get("group_execution_model") == args.execution_model
                            and r.get("group_sampling_batches") == (0 if r["name"] == "null-resource" else 1)))
                        and r["lane_comparisons"] == 189 and r["original_storage_unchanged"]
                        and r["helper"]["backend"] == args.backend for r in hardware)
                and all(r["test_passed"] and r["group_output_verified"]
                        and r.get("gradient", False) == args.grad
                        and r["masked_stages"] == ["ps", "vs", "cs"] for r in shapes))
    for run in regressions:
        run["probe_count"] = len(run.pop("probes"))
    result = {"schema": 1, "scope": "native_dxbc_sample_grad" if args.grad else "native_dxbc_sample_level", "complete_release_acceptance": False,
              "latest_validation_passed": verified, "hardware_matrix_complete": hardware_complete,
              "shape_matrix_complete": shapes_complete, "batch_matrix_complete": batch_complete,
              "independent_native_records": 25704 if args.grad else 6426,
              "runs": runs, "regressions": regressions,
              "sources": [identity(path) for path in args.source],
              "builds": [identity(path) for path in args.build],
              "binaries": [identity(path) for path in args.binary],
              "independent_evidence": [identity(path) for path in args.evidence]}
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Explicit sampling execution validated: {verified}; {len(hardware)} sampling and {len(shapes)} shape cases")
    return 0 if verified else 1


if __name__ == "__main__":
    raise SystemExit(main())
