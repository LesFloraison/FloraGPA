"""Require the complete implicit pixel sampling matrix and its adjacent regressions."""

import argparse
import itertools
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity
from summarize_dxbc_texture_uav import read_run


def validate(path, backend="dxbc-gpu/16", minimum_precision=False):
    text = path.read_text(encoding="utf-8-sig")
    rows = []
    for line in text.splitlines():
        if line.startswith("QINFO") and " {" in line:
            row = json.loads(line[line.index(" {") + 1:])
            if row.get("implicit_sampling"):
                rows.append(row)
    expected = {(v, op, config) for v, op in itertools.product((0,4318,32902),("sample","sample_b","sample_c"))
                for config in range(21 if op=="sample_c" else 14)}
    if len(rows)!=147 or {(r["vendor"],r["operation"],r["configuration"]) for r in rows}!=expected:
        raise ValueError("Incomplete implicit pixel sampling execution matrix")
    for row in rows:
        undefined = row["operation"]=="sample_c" and row["configuration"]==13
        if (row["cases"]!=64 or row["components"]!=(0 if undefined else 496)
                or row["undefined_rejections"]!=(64 if undefined else 0)
                or row["batches"]!=(0 if undefined else 64)
                or (not undefined and not 1000 <= row["steps"] <= 10000)
                or row["helper"]["backend"]!=backend
                or row["helper"]["vendor_id"]!=(row["vendor"] or 5140)
                or row["helper"]["warp"]!=(row["vendor"]==0)
                or row["model"]!="coherent-pixel-sampling/2"):
            raise ValueError("Implicit sampling assertions or execution identity changed")
    units = {f"{name}({vendor}-{opcode})" for name,vendor,opcode in itertools.product(
        ("semantics","cancellation"),(0,4318,32902),(69,70,74))} | {"unknownAndInvalid()"}
    passed = set(re.findall(r"^PASS\s+: DxbcPixelSamplingTests::((?:semantics|cancellation|unknownAndInvalid)\([^)]*\))$",text,re.MULTILINE))
    if passed!=units:
        raise ValueError("Missing implicit sampling semantic or recovery checks")
    precision_rows = []
    if minimum_precision:
        for line in text.splitlines():
            if line.startswith("QINFO") and " {" in line:
                row = json.loads(line[line.index(" {") + 1:])
                if "precision_scenario" in row:
                    precision_rows.append(row)
        expected_precision = {(v,s) for v in (0,4318,32902) for s in range(1,7)}
        if (len(precision_rows)!=18 or
                {(r["vendor"],r["precision_scenario"]) for r in precision_rows}!=expected_precision or
                any(r["cases"]!=576 or r["components"]!=4464 for r in precision_rows)):
            raise ValueError("Incomplete minimum-precision sampling matrix")
        precision_passes = set(re.findall(
            r"^PASS\s+: DxbcPixelSamplingTests::minimumPrecision\((\d+)-(\d+)\)$",text,re.MULTILINE))
        if precision_passes != {(str(v),str(s)) for v,s in expected_precision}:
            raise ValueError("Missing minimum-precision sampling checks")
    run = read_run(path)
    if not run["completed"] or run["passed"]!=(186 if minimum_precision else 168) or run["failed"] or run["skipped"]:
        raise ValueError("Implicit pixel sampling did not complete successfully")
    run.pop("probes")
    return dict(final=run,configurations=len(rows),shader_cases=sum(r["cases"] for r in rows),
                compared_components=sum(r["components"] for r in rows),
                undefined_sampler_rejections=sum(r["undefined_rejections"] for r in rows),
                sampling_cohorts=sum(r["batches"] for r in rows),steps=sum(r["steps"] for r in rows),
                unit_cases=sorted(units), minimum_precision_cases=sum(r["cases"] for r in precision_rows),
                minimum_precision_components=sum(r["components"] for r in precision_rows))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", default="dxbc-gpu/16")
    parser.add_argument("--minimum-precision", action="store_true")
    parser.add_argument("--additional-ctest", action="append", default=[])
    for name in ("log","ctest","baseline-ctest","out"):
        parser.add_argument("--"+name,type=Path,required=True)
    for name in ("regression","source","build","binary","evidence"):
        parser.add_argument("--"+name,type=Path,action="append",default=[])
    args = parser.parse_args()
    result = validate(args.log, args.backend, args.minimum_precision)
    runs,keys = [],[]
    for path in args.regression:
        run = read_run(path)
        match = re.search(r"Start testing of (\w+)",path.read_text(encoding="utf-8-sig"))
        if not match or not run["completed"] or run["failed"] or run["skipped"]:
            raise ValueError("Implicit sampling regression failed")
        keys.append((match.group(1),run["passed"]))
        run.pop("probes")
        runs.append(run)
    expected = {("DxbcPixelQuadTests",1022),("DxbcSampleComparisonTests",190),
                ("DxbcSamplingGroupTests",296),("DxbcSampleLevelTests",262),
                ("DxbcSampleLevelTests",156),("DxbcTypedPixelTests",580),
                ("DxbcGroupTests",38),("DxbcExecutionTests",375)}
    if len(keys)!=8 or set(keys)!=expected:
        raise ValueError("Missing required implicit sampling regressions")
    def times(path):
        return {name:float(seconds) for name,seconds in re.findall(
            r"\d+/\d+ Test\s+#\d+:\s+(\w+)\s+\.+\s+Passed\s+([\d.]+)\s+sec",path.read_text(encoding="utf-8-sig"))}
    current,previous = times(args.ctest),times(args.baseline_ctest)
    required = {"dxbc_pixel_sampling","dxbc_pixel_quad","dxbc_sample_comparison","dxbc_sampling_group",
                "dxbc_sample_grad","dxbc_sample_level","dxbc_typed_pixel","dxbc_group","dxbc_execution"}
    if set(current)!=required | set(args.additional_ctest):
        raise ValueError("Missing final serial implicit sampling CTest evidence")
    result.update(schema=1,scope="native_implicit_pixel_sampling",validation_passed=True,
        complete_release_acceptance=False,captured_quad_inputs_validated=False,
        helper_memory_semantics_validated=False,additional_texture_shapes_validated=False,regressions=runs,
        performance=dict(current=identity(args.ctest),baseline=identity(args.baseline_ctest),
            current_seconds=current,baseline_seconds=previous,
            ratios={k:current[k]/v for k,v in previous.items() if k in current},
            controlled_comparison=False,performance_acceptance=False),
        sources=[identity(p) for p in args.source],builds=[identity(p) for p in args.build],
        binaries=[identity(p) for p in args.binary],supporting_evidence=[identity(p) for p in args.evidence])
    args.out.write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    print("Validated 9408 implicit pixel programs, 19 semantic checks and eight regression suites")


if __name__ == "__main__":
    main()
