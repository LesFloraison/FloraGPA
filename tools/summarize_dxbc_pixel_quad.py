"""Require the complete pixel derivative execution matrix and semantic checks."""

import argparse
import itertools
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity
from summarize_dxbc_texture_uav import read_run


def validate(path, backend="dxbc-gpu/15", execution_model="coherent-pixel-sampling/2"):
    text = path.read_text(encoding="utf-8-sig")
    rows = [json.loads(line[line.index(" {") + 1:]) for line in text.splitlines()
            if line.startswith("QINFO") and " {" in line]
    expected = set(itertools.product((0, 4318, 32902), range(336)))
    if len(rows) != 1008 or {(r["vendor"], r["case"]) for r in rows} != expected:
        raise ValueError("Incomplete pixel quad execution matrix")
    for row in rows:
        index = row["case"] % 56
        # Each operation has uniform (8), discard (32), branch (8), loop (8) cases.
        if index < 8:
            coverage, discarded = (15, 3, 5, 1)[index % 4], 0
        elif index < 40:
            coverage, discarded = (15, 3, 5, 1)[((index - 8) // 4) % 4], (1, 2, 5, 15)[index % 4]
        else:
            coverage, discarded = (15, 3, 5, 1)[index % 4], 0
        if (row["components"] != (coverage & ~discarded).bit_count() * 4
                or row["batches"] != 1 or not 4 <= row["steps"] <= 1000
                or row["model"] != execution_model
                or row["adapter"]["backend"] != backend
                or row["adapter"]["vendor_id"] != (row["vendor"] or 5140)
                or row["adapter"]["warp"] != (row["vendor"] == 0)):
            raise ValueError("Pixel quad assertions or execution identity changed")
    expected_units = {f"{name}({vendor})" for name, vendor in itertools.product(
        ("validityAndModifiers", "directBits", "cancellation"), (0, 4318, 32902))}
    expected_units |= {"helperMemoryRejected()", "rendezvousBudget()", "explicitFailures()"}
    units = set(re.findall(r"^PASS\s+: DxbcPixelQuadTests::((?:validityAndModifiers|directBits|cancellation|helperMemoryRejected|rendezvousBudget|explicitFailures)\([^)]*\))$", text, re.MULTILINE))
    if units != expected_units:
        raise ValueError("Missing pixel quad validity or recovery checks")
    run = read_run(path)
    if not run["completed"] or run["passed"] != 1022 or run["failed"] or run["skipped"]:
        raise ValueError("Pixel quad execution did not complete successfully")
    run.pop("probes")
    return dict(final=run, matrix_cases=len(rows), compared_components=sum(r["components"] for r in rows),
                direct_raw_vectors=144, steps=sum(r["steps"] for r in rows), unit_cases=sorted(units))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", default="dxbc-gpu/15")
    parser.add_argument("--execution-model", default="coherent-pixel-sampling/2")
    parser.add_argument("--additional-ctest", action="append", default=[])
    for name in ("log", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--ctest", type=Path, required=True)
    parser.add_argument("--baseline-ctest", type=Path, required=True)
    for name in ("regression", "source", "build", "binary", "evidence"):
        parser.add_argument("--" + name, type=Path, action="append", default=[])
    args = parser.parse_args()
    result = validate(args.log, args.backend, args.execution_model)
    regressions, keys = [], []
    for path in args.regression:
        run = read_run(path)
        name = re.search(r"Start testing of (\w+)", path.read_text(encoding="utf-8-sig"))
        if not name or not run["completed"] or run["failed"] or run["skipped"]:
            raise ValueError("Pixel quad regression failed")
        keys.append((name.group(1), run["passed"]))
        run.pop("probes")
        regressions.append(run)
    expected = {("DxbcSampleLevelTests", 262), ("DxbcSampleLevelTests", 156),
                ("DxbcSampleComparisonTests", 190), ("DxbcSamplingGroupTests", 296),
                ("DxbcGroupTests", 38), ("DxbcExecutionTests", 375), ("DxbcTypedPixelTests", 580)}
    if len(keys) != 7 or set(keys) != expected:
        raise ValueError("Missing required pixel quad regression suites")
    def times(path):
        return {name: float(seconds) for name, seconds in re.findall(
            r"\d+/\d+ Test\s+#\d+:\s+(\w+)\s+\.+\s+Passed\s+([\d.]+)\s+sec",
            path.read_text(encoding="utf-8-sig"))}
    current, baseline = times(args.ctest), times(args.baseline_ctest)
    required_ctest = {"dxbc_pixel_quad", "dxbc_sample_comparison", "dxbc_sampling_group",
                        "dxbc_sample_grad", "dxbc_sample_level", "dxbc_typed_pixel", "dxbc_group", "dxbc_execution"}
    if set(current) != required_ctest | set(args.additional_ctest):
        raise ValueError("Missing final serial CTest completion evidence")
    performance = dict(current=identity(args.ctest), baseline=identity(args.baseline_ctest),
        current_seconds=current, baseline_seconds=baseline,
        ratios={k: current[k] / v for k, v in baseline.items() if k in current},
        controlled_comparison=False, performance_acceptance=False)
    result.update(schema=1, scope="native_pixel_derivative_execution", validation_passed=True,
                  complete_release_acceptance=False, captured_quad_inputs_validated=False,
                  helper_memory_semantics_validated=False, regressions=regressions, performance=performance,
                  sources=[identity(p) for p in args.source], builds=[identity(p) for p in args.build],
                  binaries=[identity(p) for p in args.binary], supporting_evidence=[identity(p) for p in args.evidence])
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Validated 1008 complete pixel programs, 12 semantic checks and seven regression suites")


if __name__ == "__main__":
    main()
