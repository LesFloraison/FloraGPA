"""Preserve native typed-buffer probes without claiming complete shader debugger parity."""

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
    probes, adapters = [], {}
    for line in text.splitlines():
        if not line.startswith("QINFO") or " {" not in line:
            continue
        item = json.loads(line[line.index(" {") + 1:])
        if "driver_version" in item:
            adapters[json.dumps(item, sort_keys=True)] = item
        if "DxbcTypedBufferTests::" not in line or item.get("probe") != "dxbc-typed-buffer":
            continue
        case = re.search(r"DxbcTypedBufferTests::([^ ]+)", line).group(1)
        item["case"] = case
        item["test_passed"] = bool(re.search(
            r"^PASS\s+: DxbcTypedBufferTests::" + re.escape(case) + r"$", text, re.MULTILINE))
        item["raw_output_equal"] = item["cpu_sha256"] == item["gpu_sha256"]
        probes.append(item)
    return {**identity(path), "passed": int(totals[1]), "failed": int(totals[2]),
            "skipped": int(totals[3]),
            "failure_cases": re.findall(r"^FAIL!\s+: (.*)$", text, re.MULTILINE),
            "adapters": list(adapters.values()), "probes": probes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, action="append", required=True)
    parser.add_argument("--regression", type=Path, action="append", default=[])
    parser.add_argument("--observation", type=Path, action="append", default=[])
    parser.add_argument("--source", type=Path, action="append", default=[])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    runs = [read_run(path) for path in args.log]
    regressions = [read_run(path) for path in args.regression]
    observations = [{**identity(path), "report": json.loads(path.read_text(encoding="utf-8-sig"))}
                    for path in args.observation]
    passed = runs[-1]["failed"] == 0 and runs[-1]["skipped"] == 0
    regression_passed = all(run["failed"] == 0 and run["skipped"] == 0 for run in regressions)
    observed = all(item["report"].get("observed") is True for item in observations)
    result = {"schema": 1, "scope": "native_dxbc_typed_buffer_research",
              "complete_release_acceptance": False, "latest_run_passed": passed,
              "regressions_passed": regression_passed, "regressions": regressions,
              "all_real_boundaries_observed": observed, "real_frame_observations": observations,
              "sources": [identity(path) for path in args.source], "runs": runs}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(runs)} runs; latest run passed: {passed}")
    return 0 if passed and regression_passed and observed else 1


if __name__ == "__main__":
    raise SystemExit(main())
