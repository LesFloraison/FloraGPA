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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, action="append", required=True)
    parser.add_argument("--regression", type=Path, action="append", default=[])
    parser.add_argument("--source", type=Path, action="append", default=[])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    runs = [read_run(path) for path in args.log]
    regressions = [base_run(path) for path in args.regression]
    for run in regressions:
        run["probe_count"] = len(run.pop("probes"))
    expected = set(itertools.product([0, 0x10de, 0x8086], [2, 3, 5, 7, 8], [2, 3, 10, 28, 42], range(10)))
    observed = {tuple(item[key] for key in ("vendor", "dimension", "format", "mode"))
                for item in runs[-1]["probes"]}
    complete = observed == expected and len(runs[-1]["probes"]) == len(expected)
    regression_passed = all(not run["failed"] and not run["skipped"] for run in regressions)
    required_checks_passed = all(runs[-1]["required_checks"].values())
    success = passed(runs[-1]) and complete and regression_passed and required_checks_passed
    for run in runs[:-1]:
        probes = run.pop("probes")
        run["probe_count"] = len(probes)
        run["probe_failures"] = [item for item in probes if not item["passed"] or not item["test_passed"]]
    result = {"schema": 1, "scope": "native_dxbc_texture_load_research",
              "complete_release_acceptance": False, "latest_run_passed": passed(runs[-1]),
              "hardware_matrix_complete": complete, "regressions_passed": regression_passed,
              "required_checks_passed": required_checks_passed,
              "reference_stage": "cs", "executed_stages": ["ps", "vs", "cs"], "full_vs_ps_shader_parity": False,
              "sources": [identity(path) for path in args.source],
              "runs": runs, "regressions": regressions}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(runs)} runs; full load matrix passed: {success}")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
