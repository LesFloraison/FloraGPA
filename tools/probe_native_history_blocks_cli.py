"""Compare bounded native history jobs with a retained pre-change native Worker.

GPU jobs run serially. The reference is this project's native implementation;
neither command uses RenderDoc or the GPA installation.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path

from validate_native_analysis import digest, load, run
from validate_native_history_blocks import require


def comparison(folder, capture, vendor, budget, compressed=True):
    current, reference = (load(folder / name / "result.json") for name in ("native", "reference"))
    for result in (current, reference):
        require(result["ok"] and result["context"]["frame_sha256"] == digest(capture), "result/frame identity")
        adapter = result["context"]["adapter"]
        require(adapter["warp"] == (vendor == 0) and
                adapter["vendor"] == (vendor or 5140), "explicit adapter")
        mode = "native" if result is current else "reference"
        job = load(folder / mode / "job.json")
        require(result["request_id"] == job["request_id"] and job["action"] == "history", "request identity")
        for field in ("schema", "backend", "backend_sha256", "frame_sha256", "experiment_sha256", "action", "options"):
            require(result["context"][field] == job[field], "request context: " + field)
        context = json.dumps(result["context"], sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
        require(result["context_id"] == hashlib.sha256(context).hexdigest(), "context digest")
        modules = [name.replace("\\", "/").lower() for name in result["execution"]["loaded_modules"]]
        require(modules and not any("renderdoc" in name or "/intelswtools/gpa/" in name for name in modules),
                "native loaded-module audit")
        for name, entry in result["artifacts"].items():
            artifact = folder / ("native" if result is current else "reference") / name
            require(artifact.stat().st_size == entry["bytes"] and digest(artifact) == entry["sha256"],
                    "published artifact identity")
    require(current["context"]["adapter"] == reference["context"]["adapter"], "same actual adapter/driver")
    require(load(folder / "native/job.json")["options"]["max_bytes"] == budget, "requested readback budget")
    reads = 4 if compressed else reference["data"]["readback_count"]
    require(reads > 0 and current["data"]["readback_count"] == reads and
            current["data"]["block_readback_count"] == (reads if compressed else 0) and
            current["data"]["point_readback_count"] == (0 if compressed else reads) and
            current["data"]["readback_bytes"] == budget * reads, "bounded snapshots")
    for result, scope in ((current, "single_compressed_block" if compressed else "single_texel"),
                           (reference, "complete_storage" if compressed else "single_texel")):
        require(all(record[boundary]["readback_scope"] == scope for record in result["data"]["records"]
                    for boundary in ("before", "after") if record[boundary]["available"]),
                "explicit storage readback scope")
    projections = []
    for result in (current, reference):
        projection = copy.deepcopy(result["data"])
        projection.pop("block_readback_count", None)
        projection.pop("readback_bytes")
        for record in projection["records"]:
            for boundary in ("before", "after"):
                record[boundary].pop("readback_scope", None)
        projections.append(projection)
    require(projections[0] == projections[1], "all event, raw, decoded and provenance fields match")
    return {"ok": True, "native_readback_bytes": current["data"]["readback_bytes"],
            "reference_readback_bytes": reference["data"]["readback_bytes"],
            "native_execution": current["execution"], "reference_execution": reference["execution"],
            "adapter": current["context"]["adapter"], "frame_sha256": digest(capture)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--reference-cli", type=Path, required=True)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--evidence", type=Path, help="Retained synthetic BC probe folders")
    inputs.add_argument("--captures", type=Path, help="Run GF2, BF1 and Helldivers ordinary-texture cases instead")
    parser.add_argument("--qt-bin", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--validate-only", action="store_true")
    parser.add_argument("--only-case", action="append", help="Explicit case label for a focused repeat")
    parser.add_argument("--reference-first", action="store_true", help="Reverse pair order for timing repeats")
    args = parser.parse_args()
    if not args.validate_only:
        require(not args.out.exists(), "use a new output directory")
        args.out.mkdir(parents=True)
    environment = dict(os.environ)
    environment["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + environment.get("PATH", "")
    report = {"schema": 1, "complete_release_acceptance": False, "cli_sha256": digest(args.cli),
              "reference_cli_sha256": digest(args.reference_cli),
              "worker_sha256": digest(args.cli.parent / "FloraGPA.Worker.exe"),
              "reference_worker_sha256": digest(args.reference_cli.parent / "FloraGPA.Worker.exe"),
              "requested_cases": args.only_case, "reference_first": args.reference_first, "cases": []}
    for vendor in (0, 4318, 32902):
        if args.captures:
            cases = [(f"{vendor}-{label}", args.captures / filename, resource, 0, 0, x, y, budget)
                     for label, filename, resource, x, y, budget in (
                         ("gf2", "GF2_Exilium_2026_03_03__00_19_35.gpa_frame", 80, 1280, 720, 4),
                         ("bf1", "bf1_2026_01_21__16_53_05.gpa_frame", 23209, 512, 512, 1),
                         ("helldivers", "helldivers2_2026_04_02__18_02_58.gpa_frame", 99, 128, 128, 4))]
        else:
            cases = [(f"{vendor}-{fmt}-{cube}", args.evidence / f"{vendor}-{fmt}-{cube}" / "source.gpa_frame",
                      10, mip, layer, x, y, budget)
                     for fmt, cube, mip, layer, x, y, budget in (
                         (71, 0, 1, 1, 5, 9, 8), (95, 0, 2, 1, 2, 4, 16),
                         (99, 0, 2, 1, 2, 4, 16), (71, 1, 3, 5, 0, 0, 8))]
        for name, capture, resource, mip, layer, x, y, budget in cases:
            if args.only_case and name not in args.only_case:
                continue
            capture = capture.resolve()
            folder = args.out / name
            record = {"case": name, "ok": False}
            report["cases"].append(record)
            try:
                if not args.validate_only:
                    folder.mkdir()
                    order = [("native", args.cli), ("reference", args.reference_cli)]
                    if args.reference_first:
                        order.reverse()
                    for mode, cli in order:
                        command = [str(cli.resolve()), "analyze", str(capture), "history", "--history-scope", "events",
                                   "--resource", str(resource), "--x", str(x), "--y", str(y), "--mip", str(mip),
                                   "--layer", str(layer), "--timeout", "60", "--out", str((folder / mode).resolve())]
                        command += ["--gpu-vendor", str(vendor)] if vendor else ["--warp"]
                        if mode == "native":
                            command += ["--max-bytes", str(budget)]
                        record[mode + "_wall_seconds"] = run(command, folder / (mode + ".log"), 75, environment)
                for mode, key in (("native", "worker_sha256"), ("reference", "reference_worker_sha256")):
                    require(load(folder / mode / "job.json")["backend_sha256"] == report[key],
                            "executed Worker binary identity")
                record.update(comparison(folder, capture, vendor, budget, args.captures is None))
            except Exception as error:
                record["error"] = str(error)
    require(report["cases"] and (not args.only_case or
            {case["case"] for case in report["cases"]} == set(args.only_case)), "requested cases exist")
    report["ok"] = all(case["ok"] for case in report["cases"])
    target = args.out / ("revalidation.json" if args.validate_only else "validation.json")
    require(not target.exists(), "preserve previous reports")
    target.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"ok": report["ok"], "cases": len(report["cases"]),
                      "failures": [case for case in report["cases"] if not case["ok"]]}, indent=2))
    raise SystemExit(0 if report["ok"] else 1)


if __name__ == "__main__":
    main()
