"""Record native PS typed-buffer evidence without claiming full fragment debugging."""

import argparse
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity
from summarize_dxbc_typed_vertices import read_run as graphics_run


def read_run(path):
    result = graphics_run(path)
    text = path.read_text(encoding="utf-8-sig")
    for line in text.splitlines():
        if not line.startswith("QINFO") or " {" not in line or "DxbcTypedPixelTests::" not in line:
            continue
        item = json.loads(line[line.index(" {") + 1:])
        if item.get("probe") != "dxbc-typed-pixel":
            continue
        case = re.search(r"DxbcTypedPixelTests::([^ ]+)", line).group(1)
        item["case"] = case
        item["test_passed"] = bool(re.search(
            r"^PASS\s+: DxbcTypedPixelTests::" + re.escape(case) + r"$", text, re.MULTILINE))
        item["raw_output_equal"] = item["cpu"] == item["gpu"]
        result["probes"].append(item)
    return result


def compact_regression(path):
    result = read_run(path)
    probes = result.pop("probes")
    result["probe_count"] = len(probes)
    result["probe_failures"] = [p for p in probes if not p["test_passed"] or not p["raw_output_equal"]]
    helpers = {json.dumps(p["helper"], sort_keys=True): p["helper"] for p in probes if "helper" in p}
    result["helper_identities"] = list(helpers.values())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, action="append", required=True)
    parser.add_argument("--regression", type=Path, action="append", default=[])
    parser.add_argument("--source", type=Path, action="append", default=[])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    runs = [read_run(path) for path in args.log]
    regressions = [compact_regression(path) for path in args.regression]
    passed = runs[-1]["failed"] == 0 and runs[-1]["skipped"] == 0
    regression_passed = all(run["failed"] == 0 and run["skipped"] == 0 and not run["probe_failures"]
                            for run in regressions)
    result = {"schema": 1, "scope": "native_dxbc_typed_pixel_research",
              "complete_release_acceptance": False, "latest_run_passed": passed,
              "regressions_passed": regression_passed, "regressions": regressions,
              "sources": [identity(path) for path in args.source], "runs": runs}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(runs)} runs; latest run passed: {passed}")
    return 0 if passed and regression_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
