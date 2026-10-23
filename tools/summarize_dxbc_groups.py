"""Record native DXBC group probes without treating atomic schedules as equal."""

import argparse
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity, summarize


def read_run(path):
    result = summarize(path)
    text = path.read_text(encoding="utf-8-sig")
    probes = []
    for line in text.splitlines():
        if not line.startswith("QINFO") or " {" not in line:
            continue
        item = json.loads(line[line.index(" {") + 1:])
        if "fixture" not in item or "cpu_sha256" not in item:
            continue
        case = re.search(r"hardware\(([^)]+)\)", line).group(1)
        item["case"] = case
        item["test_passed"] = bool(re.search(
            r"^PASS\s+: DxbcGroupTests::hardware\(" + re.escape(case) + r"\)$", text, re.MULTILINE))
        item["raw_output_equal"] = item["cpu_sha256"] == item["gpu_sha256"]
        probes.append(item)
    result["group_probes"] = probes
    result["group_probe_opcodes"] = sorted({opcode for item in probes for opcode in item.get("opcodes", [])})
    result["ticket_constraint_cases"] = [item["case"] for item in probes
        if item["comparison"] == "ticket_permutation_and_exact_other_bytes" and item["test_passed"]]
    result["original_gpu_schedule_recovered"] = False
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, action="append", required=True)
    parser.add_argument("--source", type=Path, action="append", default=[])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    runs = [read_run(path) for path in args.log]
    passed = all(run["failed"] == 0 and run["skipped"] == 0 for run in runs)
    result = {"schema": 1, "scope": "native_dxbc_thread_group_research",
              "complete_release_acceptance": False, "all_recorded_runs_passed": passed,
              "sources": [identity(path) for path in args.source], "runs": runs}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(runs)} runs; all passed without skips: {passed}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
