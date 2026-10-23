"""Compare native VS input replay against assembled VS output on selected real draws.

This is a migration diagnostic. Exact equality requires the selected draw to have
the same ordered references in both modes; it is not a complete mesh parity gate.
GPU jobs run serially, using only FloraGPA's native Worker.
"""
import argparse
import json
import os
from pathlib import Path
import sys

from validate_native_analysis import digest, load, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--case", nargs=2, action="append", metavar=("CAPTURE", "EVENT"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--qt-bin", type=Path)
    parser.add_argument("--gpu-vendor", default="0x10de")
    parser.add_argument("--instance", type=int, default=0)
    parser.add_argument("--reference-stage", choices=("vs-index", "vs"), default="vs-index")
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    if args.out.exists():
        parser.error("Output directory must be new")
    args.out.mkdir(parents=True)
    environment = os.environ.copy()
    if args.qt_bin:
        environment["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + environment.get("PATH", "")
    rows = []
    for index, (capture, event) in enumerate(args.case):
        row = {"capture": str(Path(capture).resolve()), "event": int(event), "instance": args.instance,
               "reference_stage": args.reference_stage, "identity_comparison": args.reference_stage == "vs-index"}
        rows.append(row)
        root = args.out / str(index)
        root.mkdir()
        try:
            reports = {}
            for stage in ("vs-inputs", args.reference_stage):
                directory = root / stage
                wall = run([str(args.cli.resolve()), "analyze", row["capture"], "mesh", "--gpa-event", event,
                            "--stage", stage, "--instance", str(args.instance), "--gpu-vendor", args.gpu_vendor,
                            "--timeout", str(args.timeout), "--out", str(directory.resolve())],
                           root / (stage + ".log"), args.timeout + 30, environment)
                report = load(directory / "result.json")
                if not report["ok"] or report["context"]["adapter"]["vendor"] != int(args.gpu_vendor, 0):
                    raise RuntimeError("Native analysis failed or used another adapter")
                for name, expected in report["artifacts"].items():
                    path = directory / name
                    if path.stat().st_size != expected["bytes"] or digest(path) != expected["sha256"]:
                        raise RuntimeError("Artifact identity mismatch: " + name)
                modules = [name.lower() for name in report["execution"]["loaded_modules"]]
                if any("renderdoc" in name or "intelswtools\\gpa\\" in name for name in modules):
                    raise RuntimeError("External analysis module loaded")
                row[stage] = {"vertices": report["data"]["vertices"],
                              "unique_vertices": report["data"].get("unique_vertices"),
                              "raw_sha256": report["data"]["sha256"], "wall_seconds": wall,
                              "execution": report["execution"], "context": report["context"]}
                reports[stage] = report
            left, right = root / "vs-inputs", root / args.reference_stage
            row["identities_equal"] = (load(left / "identities.json") == load(right / "identities.json")
                                       if args.reference_stage == "vs-index" else None)
            row["raw_bytes_equal"] = (left / "vertices.bin").read_bytes() == (right / "vertices.bin").read_bytes()
            row["attributes_equal"] = reports["vs-inputs"]["data"]["attributes"] == reports[args.reference_stage]["data"]["attributes"]
            a, b = reports["vs-inputs"]["context"], reports[args.reference_stage]["context"]
            row["source_context_equal"] = all(a[key] == b[key] for key in
                                                ("frame_sha256", "experiment_sha256", "adapter", "backend_sha256"))
            row["ok"] = all(row[key] for key in ("raw_bytes_equal", "attributes_equal", "source_context_equal"))
            if args.reference_stage == "vs-index":
                row["ok"] = row["ok"] and row["identities_equal"]
        except Exception as error:
            row.update(ok=False, error=str(error))
        (args.out / "comparison.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
        print(f"Case {index}, GPA event {event}: {'matched' if row['ok'] else 'needs investigation'}", flush=True)
    return 0 if all(row["ok"] for row in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
