"""Require native completion recovery and every affected DXBC regression suite."""

import argparse
import itertools
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity
from summarize_dxbc_texture_uav import read_run

REGRESSIONS = {
    "pixel_sampling": ("DxbcPixelSamplingTests", 168), "pixel_quad": ("DxbcPixelQuadTests", 1022),
    "sample_comparison": ("DxbcSampleComparisonTests", 190), "sampling_group": ("DxbcSamplingGroupTests", 296),
    "sample_grad": ("DxbcSampleLevelTests", 262), "sample_level": ("DxbcSampleLevelTests", 156),
    "sample_query": ("DxbcSampleQueryTests", 375), "multisample": ("DxbcMultisampleTests", 105),
    "texture_uav": ("DxbcTextureUavTests", 765), "resource_info": ("DxbcResourceInfoTests", 629),
    "texture_load": ("DxbcTextureLoadTests", 1445), "texture_input": ("DxbcTextureInputTests", 76),
    "typed_pixel": ("DxbcTypedPixelTests", 580), "typed_vertex": ("DxbcTypedVertexTests", 580),
    "typed_buffer": ("DxbcTypedBufferTests", 473), "input_snapshot": ("DxbcInputSnapshotTests", 82),
    "counter": ("DxbcCounterTests", 40), "group": ("DxbcGroupTests", 38),
    "memory": ("DxbcMemoryTests", 246), "execution": ("DxbcExecutionTests", 375), "format": ("DxbcFormatTests", 16),
}


def validate(path):
    text = path.read_text(encoding="utf-8-sig")
    rows = [json.loads(line[line.index(" {") + 1:]) for line in text.splitlines()
            if line.startswith("QINFO") and " {" in line]
    expected = set(itertools.product((0, 4318, 32902), (False, True)))
    for key in ("completion_readbacks", "completion_cancellation", "handle_observation"):
        selected = [r for r in rows if r.get(key)]
        if len(selected) != 6 or {(r["vendor"], r["poll"]) for r in selected} != expected:
            raise ValueError("Incomplete native completion recovery matrix")
        for row in selected:
            if key == "completion_readbacks" and (row["copies"] != 32 or row["queries"] != 32
                    or row["subresources"] != 4 or row["fence"] == row["poll"]):
                raise ValueError("Missing completion readback or query assertions")
            if key == "completion_cancellation" and (row["cancelled"] != 64 or row["retries"] != 64
                                                      or row["handle_delta"] > 4):
                raise ValueError("Missing cancellation recovery or excessive retained handles")
            if key == "handle_observation" and (len(row["counts"]) != 64
                    or len(row["completed_control_counts"]) != 32 or row["after"] > row["before"] + 4):
                raise ValueError("Missing handle lifetime controls")
    stages = [r for r in rows if r.get("completion_stage")]
    if len(stages) != 18 or {(r["vendor"], r["poll"], r["stage"]) for r in stages} != {
            (*key, stage) for key in expected for stage in (0, 1, 5)}:
        raise ValueError("Incomplete completion shader stage matrix")
    for row in stages:
        helper = row["helper"]
        if (row["assisted"] != (2 if row["stage"] == 5 else 1) or helper["backend"] != "dxbc-gpu/16"
                or helper["completion_wait"] != ("native-poll/1" if row["poll"] else "fence-event/1")
                or helper["vendor_id"] != (row["vendor"] or 5140) or helper["warp"] != (row["vendor"] == 0)):
            raise ValueError("Completion stage changed its execution source or identity")
    units = {f"deadlines({vendor}-{'poll' if poll else 'event'})" for vendor, poll in expected} | {"invalid()"}
    actual = set(re.findall(r"^PASS\s+: DxbcGpuCompletionTests::((?:deadlines|invalid)\([^)]*\))$", text, re.MULTILINE))
    final = read_run(path)
    if units != actual or not final["completed"] or final["passed"] != 39 or final["failed"] or final["skipped"]:
        raise ValueError("Native GPU completion did not pass every recovery and deadline check")
    final.pop("probes")
    return dict(final=final, readback_cases=6, stage_cases=18, cancelled_submissions=384,
                fresh_retries=384, lifetime_controls=192, unit_cases=sorted(units))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("log", "baseline-ctest", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--ctest", type=Path, action="append", required=True)
    parser.add_argument("--execution", type=Path, action="append", required=True)
    for name in ("regression", "source", "binary", "evidence"):
        parser.add_argument("--" + name, type=Path, action="append", default=[])
    args = parser.parse_args()
    result = validate(args.log)
    regressions, keys = [], []
    for path in args.regression:
        run = read_run(path)
        match = re.search(r"Start testing of (\w+)", path.read_text(encoding="utf-8-sig"))
        if not match or not run["completed"] or run["failed"] or run["skipped"]:
            raise ValueError("Completion regression did not finish successfully")
        keys.append((match[1], run["passed"]))
        run.pop("probes")
        regressions.append(run)
    if len(keys) != 21 or set(keys) != set(REGRESSIONS.values()):
        raise ValueError("Missing required completion regression suites or assertions")
    def times(path):
        return {name: float(seconds) for name, seconds in re.findall(
            r"\d+/\d+ Test\s+#\d+:\s+(\w+)\s+\.+\s+Passed\s+([\d.]+)\s+sec", path.read_text(encoding="utf-8-sig"))}
    current, previous = {}, times(args.baseline_ctest)
    for part in args.ctest:
        observed = times(part)
        if current.keys() & observed.keys():
            raise ValueError("Duplicate completed suites in completion CTest evidence")
        current.update(observed)
    if set(current) != {"dxbc_gpu_completion"} | {"dxbc_" + name for name in REGRESSIONS}:
        raise ValueError("Incomplete final completion CTest evidence")
    captured, observed = [], set()
    expected_captures = {
        "26044d33cb6c3087c2204b7d57664203741a679f988430b7d05b24af582e0620": (209, 190, 14992, 2000, 1488),
        "97915a80c7617e9a67068a5d85b87f1b2a7883faa9831428084ebd7270f37139": (893, 891, 384, 64, 64),
    }
    for path in args.execution:
        report = json.loads(path.read_text(encoding="utf-8-sig"))
        frame = report["frame_sha256"]
        execution = report["execution"]
        if (frame not in expected_captures or frame in observed or not report["observed"]
                or report["schema"] != 6 or report["stage"] != 5 or not execution["completed"]
                or execution["group"] != [0, 0, 0] or execution["model"] != "bounded-sampling-rendezvous/3"
                or report["source"] != "independent-replay-resource-inputs/7"
                or execution["helper"]["backend"] != "dxbc-gpu/16"
                or execution["helper"]["completion_wait"] != "fence-event/1"
                or execution["helper"]["vendor_id"] != 4318
                or (report["event"], report["shader"], execution["steps"], execution["gpu_operations"],
                    execution["texture_writes"]) != expected_captures[frame]):
            raise ValueError("Incomplete captured shader completion evidence")
        if report["event"] == 893:
            comparison = execution["native_comparison"]
            if (not comparison["attempted"] or not comparison["passed"]
                    or not comparison["matching_native_inputs"]
                    or not comparison["original_observed_resources_unchanged"]
                    or comparison["scope"] != "single_direct_dispatch_observed_resources"
                    or len(comparison["resources"]) != 1 or comparison["resources"][0]["resource"] != 886
                    or any(not r["equal"] or r["expected_sha256"] != r["native_sha256"] for r in comparison["resources"])):
                raise ValueError("Captured single-dispatch output comparison failed")
        observed.add(frame)
        captured.append(dict(report=identity(path), frame_sha256=frame, event=report["event"],
                             shader=report["shader"], execution=execution))
    if observed != expected_captures.keys():
        raise ValueError("Missing required captured shader completion runs")
    result.update(schema=1, scope="native_gpu_completion", validation_passed=True, complete_release_acceptance=False,
        process_recovery_acceptance=False, device_removal_simulated=False, unsupported_driver_tested=False,
        regressions=regressions, captured_executions=captured,
        total_passed=39 + sum(run["passed"] for run in regressions),
        performance=dict(current=[identity(p) for p in args.ctest], baseline=identity(args.baseline_ctest),
            current_seconds=current, baseline_seconds=previous,
            ratios={k: current[k] / v for k, v in previous.items() if k in current},
            controlled_comparison=False, performance_acceptance=False),
        sources=[identity(p) for p in args.source], binaries=[identity(p) for p in args.binary],
        evidence=[identity(p) for p in args.evidence])
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Validated native completion and {result['total_passed']} tests across 22 serial suites")


if __name__ == "__main__":
    main()
