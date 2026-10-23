"""Preserve native DXBC hidden-counter probes and their explicit comparison scope."""

import argparse
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity


def read_run(path):
    text = path.read_text(encoding="utf-8-sig")
    totals = re.search(r"Totals: (\d+) passed, (\d+) failed, (\d+) skipped", text)
    if totals is None:
        raise ValueError(f"Missing completed test totals: {path}")
    probes = []
    for line in text.splitlines():
        if not line.startswith("QINFO") or " {" not in line:
            continue
        item = json.loads(line[line.index(" {") + 1:])
        if not item.get("probe", "").startswith("dxbc-hidden-counter"):
            continue
        case = re.search(r"DxbcCounterTests::([^ ]+)", line).group(1)
        item["case"] = case
        item["test_passed"] = bool(re.search(
            r"^PASS\s+: DxbcCounterTests::" + re.escape(case) + r"$", text, re.MULTILINE))
        item["raw_output_equal"] = item["cpu_output_sha256"] == item["gpu_output_sha256"]
        if "cpu_storage_sha256" in item:
            item["raw_storage_equal"] = item["cpu_storage_sha256"] == item["gpu_storage_sha256"]
        probes.append(item)
    return {**identity(path), "passed": int(totals[1]), "failed": int(totals[2]),
            "skipped": int(totals[3]),
            "failure_cases": re.findall(r"^FAIL!\s+: (.*)$", text, re.MULTILINE),
            "probes": probes, "original_gpu_schedule_recovered": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, action="append", required=True)
    parser.add_argument("--source", type=Path, action="append", default=[])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    runs = [read_run(path) for path in args.log]
    passed = all(run["failed"] == 0 and run["skipped"] == 0 for run in runs)
    result = {"schema": 1, "scope": "native_dxbc_hidden_counter_research",
              "complete_release_acceptance": False, "all_recorded_runs_passed": passed,
              "sources": [identity(path) for path in args.source], "runs": runs}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(runs)} runs; all passed without skips: {passed}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
