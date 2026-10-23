"""Validate native inventory and module provenance without any external replay tool.

An optional frozen FloraGPA baseline enables a development-only inventory comparison.
This is a focused migration probe, not the complete release acceptance suite.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(arguments, log, timeout, environment):
    start = time.monotonic()
    with log.open("wb") as output:
        result = subprocess.run(arguments, stdout=output, stderr=subprocess.STDOUT,
                                env=environment, timeout=timeout, check=False)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); see {log}")
    return time.monotonic() - start


def inventory_projection(value):
    # GPA IDs were decimal strings in the original CLI, while native schema 1 uses uint64.
    output = {key: value[key] for key in ("width", "height", "draws", "dispatches", "debug_names")}
    for key in ("resources", "commands"):
        output[key] = []
        for row in value[key]:
            normalized = dict(row)
            for identifier in ("id", "state", "context"):
                if identifier in normalized:
                    normalized[identifier] = int(normalized[identifier])
            output[key].append(normalized)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--capture", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--baseline-cli", type=Path)
    parser.add_argument("--qt-bin", type=Path)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    if args.out.exists() and any(args.out.iterdir()):
        parser.error("Output directory must be new or empty")
    args.out.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    if args.qt_bin:
        environment["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + environment.get("PATH", "")
    report = {"schema": 1, "scope": "native_inventory_and_loaded_modules", "complete_release_acceptance": False,
              "cli_sha256": digest(args.cli), "captures": []}
    for index, capture in enumerate(args.capture):
        case = args.out / str(index)
        case.mkdir()
        elapsed = run([str(args.cli.resolve()), "analyze", str(capture.resolve()), "inventory", "--out",
                       str((case / "native").resolve()), "--timeout", str(args.timeout)],
                      case / "native.log", args.timeout + 30, environment)
        result = load(case / "native/result.json")
        if result["ok"] is not True or result["context"]["frame_sha256"] != digest(capture):
            raise AssertionError("Native result does not identify the requested frame")
        if result["context"]["adapter"] is not None:
            raise AssertionError("Metadata inspection unexpectedly created a GPU device")
        modules = result["execution"]["loaded_modules"]
        if not modules:
            raise AssertionError("Missing module audit")
        for module in modules:
            normalized = module.replace("\\", "/").lower()
            if ("renderdoc" in normalized or "/intelswtools/gpa/" in normalized or
                    normalized.rsplit("/", 1)[-1] in {"dx11_player.dll", "dx11_playback.dll"}):
                raise AssertionError(f"External replay dependency loaded: {module}")
        row = {"capture": str(capture.resolve()), "frame_sha256": result["context"]["frame_sha256"],
               "wall_seconds": elapsed, "execution": result["execution"], "inventory_matches_baseline": None}
        if args.baseline_cli:
            row["baseline_wall_seconds"] = run(
                [str(args.baseline_cli.resolve()), "inventory", str(capture.resolve()), "--out",
                 str((case / "baseline").resolve())], case / "baseline.log", args.timeout, environment)
            baseline = load(case / "baseline/report.json")
            if inventory_projection(baseline) != inventory_projection(result["data"]):
                raise AssertionError(f"Inventory differs from frozen baseline for {capture}")
            row["inventory_matches_baseline"] = True
        report["captures"].append(row)
    report["ok"] = True
    (args.out / "validation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(args.out / "validation.json")


if __name__ == "__main__":
    main()
