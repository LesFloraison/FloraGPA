"""Preserve native texture-load comparisons and their bounded checkpoint scope."""

import argparse
import itertools
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity
from summarize_dxbc_texture_inputs import read_run as base_run


def read_run(path):
    result = base_run(path)
    result["probes"] = []
    helpers = []
    text = path.read_text(encoding="utf-8-sig")
    result["required_checks"] = {
        name: bool(re.search(r"^PASS\s+: DxbcTextureLoadTests::" + name + r"\(\)$", text, re.MULTILINE))
        for name in ("validitySelectionAndRecovery", "missingBindingsStillValidateInstructions")}
    result["format_validity_checks"] = re.findall(
        r"^PASS\s+: DxbcTextureLoadTests::formatValidity\(([^)]+)\)$", text, re.MULTILINE)
    for line in text.splitlines():
        if not line.startswith("QINFO") or " {" not in line:
            continue
        item = json.loads(line[line.index(" {") + 1:])
        if item.get("probe") != "dxbc-texture-load":
            continue
        case = re.search(r"DxbcTextureLoadTests::([^ ]+)", line).group(1)
        item["case"] = case
        item["test_passed"] = bool(re.search(
            r"^PASS\s+: DxbcTextureLoadTests::" + re.escape(case) + r"$", text, re.MULTILINE))
        if item.pop("reference_stage") != "cs" or item.pop("executed_stages") != ["ps", "vs", "cs"]:
            raise ValueError(f"Unexpected texture-load stage coverage: {path}")
        item.pop("probe")
        helper = item.pop("helper")
        if helper not in helpers:
            helpers.append(helper)
        item["helper_index"] = helpers.index(helper)
        result["probes"].append(item)
    result["helpers"] = helpers
    return result


def passed(run):
    return not run["failed"] and not run["skipped"] and all(
        item["passed"] and item["test_passed"] for item in run["probes"])


def execution_report(path):
    report = json.loads(path.read_text(encoding="utf-8-sig"))
    if report.get("schema") not in (3, 4) or report.get("scope") != "native_dxbc_compute_group_probe":
        raise ValueError(f"Unexpected execution probe schema: {path}")
    return {**identity(path), **{key: report.get(key) for key in (
        "schema", "capture", "event", "stage", "frame_sha256", "shader", "shader_sha256", "source", "multisample_inputs",
        "adapter_vendor", "adapter_device", "adapter_luid", "observed", "error",
        "shader_execution_performed", "unavailable", "execution", "elapsed_ms")}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, action="append", required=True)
    parser.add_argument("--regression", type=Path, action="append", default=[])
    parser.add_argument("--source", type=Path, action="append", default=[])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--extended-formats", action="store_true")
    parser.add_argument("--execution", type=Path, action="append", default=[])
    args = parser.parse_args()
    runs = [read_run(path) for path in args.log]
    regressions = [base_run(path) for path in args.regression]
    executions = [execution_report(path) for path in args.execution]
    for run in regressions:
        run["probe_count"] = len(run.pop("probes"))
    expected = set(itertools.product([0, 0x10de, 0x8086], [2, 3, 5, 7, 8], [2, 3, 10, 28, 42], range(10)))
    if args.extended_formats:
        expected |= set(itertools.product(
            [0, 0x10de, 0x8086], [3, 8],
            [21, 22, 29, 46, 47, 65, 67, 71, 72, 74, 75, 77, 78, 80, 81, 83, 84,
             85, 86, 87, 88, 91, 93, 95, 96, 98, 99, 115], [0, 10, 11, 12]))
    observed = {tuple(item[key] for key in ("vendor", "dimension", "format", "mode"))
                for item in runs[-1]["probes"]}
    complete = observed == expected and len(runs[-1]["probes"]) == len(expected)
    regression_passed = all(not run["failed"] and not run["skipped"] for run in regressions)
    required_checks_passed = all(runs[-1]["required_checks"].values())
    if args.extended_formats:
        expected_validity = {
            "bc3-alpha-independent", "bc3-color-independent", "bc5-red-independent",
            "bc5-green-independent", "bc4-default-components", "bc6-default-alpha", "bc7-shared-block",
            "bgra-byte-order", "bgrx-unused-byte", "alpha-only", "stencil-ignores-depth",
            "stencil-green-component", "depth-ignores-stencil", "depth-red-component", "depth-unused-padding",
            "shared-exponent", "srgb-alpha-independent", "b565-shared-byte", "b4444-shared-byte"}
        required_checks_passed &= set(runs[-1]["format_validity_checks"]) == expected_validity
    success = passed(runs[-1]) and complete and regression_passed and required_checks_passed
    for run in runs[:-1]:
        probes = run.pop("probes")
        run["probe_count"] = len(probes)
        run["probe_failures"] = [item for item in probes if not item["passed"] or not item["test_passed"]]
    result = {"schema": 1, "scope": "native_dxbc_texture_load_research",
              "extended_formats": args.extended_formats,
              "complete_release_acceptance": False, "latest_run_passed": passed(runs[-1]),
              "hardware_matrix_complete": complete, "regressions_passed": regression_passed,
              "required_checks_passed": required_checks_passed,
              "reference_stage": "cs", "executed_stages": ["ps", "vs", "cs"], "full_vs_ps_shader_parity": False,
              "sources": [identity(path) for path in args.source],
              "runs": runs, "regressions": regressions, "executions": executions}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(runs)} runs; full load matrix passed: {success}")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
