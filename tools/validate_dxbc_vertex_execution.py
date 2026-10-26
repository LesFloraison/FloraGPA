"""Compare selected C++ VS execution with original-shader native stream output.

Run GPU probes serially. This checks the supplied selections, not complete shader
or release parity. Captures and raw reports remain outside version control.
"""
import argparse
import json
import os
from pathlib import Path

from validate_native_analysis import digest, load, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--case", nargs=2, action="append", metavar=("CAPTURE", "EVENT"), required=True)
    parser.add_argument("--vendor", type=lambda value: int(value, 0), action="append")
    parser.add_argument("--reference", type=int, default=0)
    parser.add_argument("--instance", type=int, default=0)
    parser.add_argument("--qt-bin", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=150)
    args = parser.parse_args()
    if args.out.exists():
        parser.error("Output directory must be new")
    if min(args.reference, args.instance) < 0 or args.timeout <= 0:
        parser.error("Selections must be nonnegative and timeout positive")
    args.out.mkdir(parents=True)
    environment = os.environ.copy()
    if args.qt_bin:
        environment["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + environment.get("PATH", "")
    result = {"schema": 1, "scope": "selected-native-vertex-execution",
              "complete_release_acceptance": False, "probe_sha256": digest(args.probe), "cases": []}
    for index, (capture, event) in enumerate(args.case):
        for vendor in args.vendor or [4318, 32902, 0]:
            path = args.out / f"{index}-{vendor}.json"
            row = {"capture": str(Path(capture).resolve()), "event": int(event), "vendor": vendor,
                   "reference": args.reference, "instance": args.instance, "report": path.name}
            result["cases"].append(row)
            try:
                row["wall_seconds"] = run(
                    [str(args.probe.resolve()), row["capture"], event, str(args.reference),
                     str(args.instance), str(vendor), str(path.resolve()), "--compare-native"],
                    path.with_suffix(".log"), args.timeout, environment)
                report = load(path)
                row["report_sha256"] = digest(path)
                row["frame_sha256"] = report["frame_sha256"]
                row["shader_sha256"] = report["shader_sha256"]
                row["steps"] = report["execution_steps"]
                row["gpu_identity"] = helper = report["execution_gpu_identity"]
                row["gpu_operations"] = report["execution_gpu_operations"]
                comparison = report["native_comparison"]
                row["components"] = len(comparison["components"])
                row["different_components"] = comparison["different_components"]
                row["unknown_components"] = comparison["unknown_components"]
                luid = ((helper["luid_high"] & 0xffffffff) << 32) | helper["luid_low"]
                adapter_matches = (
                    report["adapter_vendor"] == (vendor or 5140)
                    and helper["vendor_id"] == report["adapter_vendor"]
                    and helper["device_id"] == report["adapter_device"]
                    and helper["warp"] == (vendor == 0) and luid == report["adapter_luid"])
                row["ok"] = bool(
                    adapter_matches and report["status"] == "ok" and report["input_observation_complete"]
                    and report["execution_complete"] and row["components"] > 0
                    and comparison["exact"] and row["different_components"] == 0
                    and row["unknown_components"] == 0
                    and all(item["exact"] and item["executor_valid"]
                            and item["executor_bits"] == item["native_bits"]
                            for item in comparison["components"]))
            except Exception as error:
                row.update(ok=False, error=str(error))
            result["ok"] = all(item["ok"] for item in result["cases"])
            (args.out / "comparison.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
            print(f"Case {index}, event {event}, vendor {vendor}: "
                  f"{'matched' if row['ok'] else 'needs investigation'}", flush=True)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
