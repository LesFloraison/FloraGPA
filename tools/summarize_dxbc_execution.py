"""Preserve strict DXBC execution probe results and reproducible file identities."""

import argparse
import hashlib
import json
from pathlib import Path
import re


def identity(path):
    return {"path": path.as_posix(), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def summarize(path):
    contents = path.read_text(encoding="utf-8-sig")
    totals = re.search(r"Totals: (\d+) passed, (\d+) failed, (\d+) skipped", contents)
    if totals is None:
        raise ValueError(f"Missing completed test totals: {path}")
    adapters, observations = {}, []
    for line in contents.splitlines():
        if not line.startswith("QINFO") or " {" not in line:
            continue
        item = json.loads(line[line.index(" {") + 1 :])
        if "adapter" in item:
            adapters[json.dumps(item, sort_keys=True)] = item
        if "cpu" in item and "gpu" in item:
            item["case"] = re.search(r"rawInstructions\(([^)]+)\)", line).group(1)
            observations.append(item)
    mismatches = [item for item in observations if item["cpu"] != item["gpu"]]
    return {
        **identity(path),
        "passed": int(totals[1]),
        "failed": int(totals[2]),
        "skipped": int(totals[3]),
        "failure_cases": re.findall(r"^FAIL!\s+: (.*)$", contents, re.MULTILINE),
        "adapters": list(adapters.values()),
        "raw_comparisons": len(observations),
        "mismatches": mismatches,
        "same_operation_other_forms": [
            item
            for item in observations
            if item["cpu"] == item["gpu"]
            and any(
                all(item.get(key) == mismatch.get(key) for key in ("vendor", "opcode", "output", "aliased"))
                for mismatch in mismatches
            )
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, action="append", required=True)
    parser.add_argument("--source", type=Path, action="append", default=[])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    reports = [summarize(path) for path in args.log]
    result = {
        "schema": 1,
        "scope": "native_dxbc_execution_core_research",
        "complete_release_acceptance": False,
        "latest_run_passed": reports[-1]["failed"] == 0 and reports[-1]["skipped"] == 0,
        "sources": [identity(path) for path in args.source],
        "runs": reports,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(reports)} completed runs; latest run: "
          f"{reports[-1]['passed']} passed, {reports[-1]['failed']} failed, "
          f"{reports[-1]['skipped']} skipped")
    return 0 if result["latest_run_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
