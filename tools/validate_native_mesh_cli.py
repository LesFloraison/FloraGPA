"""Run native mesh CLI jobs and verify their published exports without a presentation probe."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from compare_native_mesh_baseline import compare as compare_baseline
from validate_native_mesh_captures import CAPTURES, digest, load, require, selected_cases, validate_modules


def context(folder, backend, worker_hash):
    job, result = load(folder / "job.json"), load(folder / "result.json")
    require(result["ok"] is True and result["backend"] == backend and
            result["request_id"] == job["request_id"], "completed CLI request")
    for key in ("schema", "backend", "backend_sha256", "frame_sha256", "experiment_sha256", "action", "options"):
        require(result["context"][key] == job[key], "CLI context: " + key)
    require(job["backend_sha256"] == worker_hash and job["backend"] == backend and
            result["context_id"] == digest(json.dumps(result["context"], sort_keys=True,
                                                      separators=(",", ":"), ensure_ascii=False).encode()),
            "CLI backend and context digest")
    validate_modules(result["execution"]["loaded_modules"])
    for name, entry in result["artifacts"].items():
        require(Path(name).name == name, "artifact name")
        raw = (folder / name).read_bytes()
        require(len(raw) == entry["bytes"] and digest(raw) == entry["sha256"], "published artifact: " + name)
    return job, result


def verify(folder, reference, worker_hash, previous_hash, label, event, instance, vendor):
    job, result = context(folder, "dx11-native/11", worker_hash)
    old_job, old = context(reference, "dx11-native/10", previous_hash)
    require(job["action"] == "mesh" and job["options"]["gpa_event"] == event and
            job["options"]["instance"] == instance and job["options"]["stage"] == "vs-inputs" and
            job["options"]["vertex_storage"] == "remaining" and job["frame_sha256"] == CAPTURES[label][1],
            "CLI frame and selection")
    require(result["context"]["adapter"]["vendor"] == (vendor or 5140) and
            result["context"]["adapter"]["warp"] == (vendor == 0), "CLI adapter")
    for key in ("frame_sha256", "experiment_sha256", "options", "adapter"):
        require(result["context"][key] == old["context"][key], "reference scope: " + key)
    require(result["mesh_presentation"] == {"schema": 1, "available": True, "reason": None},
            "explicit presentation availability")
    require(result["data"] == old["data"], "raw capture metadata is unchanged")
    for name in old["artifacts"]:
        require(result["artifacts"][name] == old["artifacts"][name], "canonical artifact unchanged: " + name)
    for name in ("post_vertices.bin", "post_vertices.csv", "post_geometry.obj", "presentation.json"):
        require(name in result["artifacts"] and (folder / name).read_bytes() == (reference / name).read_bytes(),
                "CLI publication differs from prior native UI presentation: " + name)
    return {"case": f"{vendor}-{label}-{event}-{instance}", "context_id": result["context_id"],
            "artifacts": len(result["artifacts"]), "selected_vertices": result["data"]["vertices"],
            "stored_vertices": result["data"]["vertex_storage"]["vertices"],
            "execution": result["execution"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cli", "qt-bin", "captures", "reference", "baseline", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args()
    report_path = args.out / ("revalidation.json" if args.validate_only else "validation.json")
    require(not report_path.exists(), "Validation report must be new")
    if not args.validate_only:
        args.out.mkdir(parents=True, exist_ok=False)
    for filename, sha in CAPTURES.values():
        with (args.captures / filename).open("rb") as file:
            require(hashlib.file_digest(file, "sha256").hexdigest() == sha, "capture identity")
    worker_hash = digest((args.cli.parent / "FloraGPA.Worker.exe").read_bytes())
    previous_hash = load(args.reference / "validation.json")["worker_sha256"]
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    env["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + env["PATH"]
    records = []
    for vendor in (4318, 32902, 0):
        for label, event, instance in selected_cases("all"):
            name = f"{vendor}-{label}-{event}-{instance}"
            folder = args.out / name / "vs-inputs"
            command = [str(args.cli.resolve()), "analyze", str((args.captures / CAPTURES[label][0]).resolve()),
                       "mesh", "--gpa-event", str(event), "--stage", "vs-inputs", "--instance", str(instance),
                       "--vertex-storage", "remaining", "--out", str(folder.resolve()), "--timeout", "180"]
            command += ["--gpu-vendor", str(vendor)] if vendor else ["--warp"]
            record = {"case": name, "command": command, "passed": False}
            try:
                if not args.validate_only:
                    completed = subprocess.run(command, env=env, capture_output=True, timeout=190)
                    (args.out / (name + ".stdout.txt")).write_bytes(completed.stdout)
                    (args.out / (name + ".stderr.txt")).write_bytes(completed.stderr)
                    record["exit_code"] = completed.returncode
                    require(completed.returncode == 0, "CLI failed: " + completed.stderr.decode(errors="replace"))
                else:
                    original = next(c for c in load(args.out / "validation.json")["cases"] if c["case"] == name)
                    record["exit_code"] = original["exit_code"]
                    require(record["exit_code"] == 0, "original CLI did not complete")
                record.update(verify(folder, args.reference / name / "vs-inputs", worker_hash, previous_hash,
                                     label, event, instance, vendor))
                record["passed"] = True
            except (ValueError, KeyError, OSError, subprocess.TimeoutExpired) as error:
                record["error"] = str(error)
            records.append(record)
            print(name, "PASS" if record["passed"] else record["error"], flush=True)
    controls = []
    if all(c["passed"] for c in records):
        source = args.out / "4318-gf2-113-0/vs-inputs"
        for target in ("post_vertices.bin", "post_vertices.csv", "post_geometry.obj", "presentation.json", "status"):
            with tempfile.TemporaryDirectory(prefix="flora-cli-mesh-") as temporary:
                folder = Path(temporary) / "copy"
                shutil.copytree(source, folder)
                result = load(folder / "result.json")
                if target == "status":
                    result.pop("mesh_presentation")
                else:
                    raw = (folder / target).read_bytes()
                    raw = bytes([raw[0] ^ 1]) + raw[1:]
                    (folder / target).write_bytes(raw)
                    result["artifacts"][target] = {"bytes": len(raw), "sha256": digest(raw)}
                (folder / "result.json").write_text(json.dumps(result), encoding="utf-8")
                try:
                    verify(folder, args.reference / "4318-gf2-113-0/vs-inputs", worker_hash, previous_hash,
                           "gf2", 113, 0, 4318)
                except (ValueError, KeyError):
                    controls.append(target)
                else:
                    raise ValueError("Accepted corrupt published " + target)
    comparisons = []
    for case in selected_cases("all"):
        try:
            comparisons.append(compare_baseline(args.baseline, args.out, *case))
        except (ValueError, KeyError, OSError) as error:
            comparisons.append({"case": "-".join(map(str, case)), "strict_exports_equal": False,
                                "indices_equal": False, "referenced_records_equal": False, "error": str(error)})
    passed = all(c["passed"] for c in records) and len(controls) == 5 and all(
        c["strict_exports_equal"] and c["indices_equal"] and c["referenced_records_equal"] for c in comparisons)
    report_path.write_text(json.dumps({"schema": 1, "completed": True, "passed": passed,
                                       "cli_sha256": digest(args.cli.read_bytes()), "worker_sha256": worker_hash,
                                       "reference_worker_sha256": previous_hash, "cases": records,
                                       "corruption_controls": controls, "baseline_comparison": comparisons}, indent=2) + "\n",
                           encoding="utf-8")
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
