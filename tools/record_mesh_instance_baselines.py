"""Extend retained frozen-main mesh evidence with distinct instance selections.

Development-only: the three new baseline jobs explicitly use RenderDoc. Native
validation and saved-report comparison are separate tools without this dependency.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from validate_native_mesh_captures import ADDITIONAL_CASES, CASES, load, require


def identity(path):
    with path.open("rb") as stream:
        sha = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": path.resolve().as_posix(), "bytes": path.stat().st_size, "sha256": sha}


def label(case):
    name, event, instance = case
    return "gf2-113" if case == ("gf2", 113, 0) else f"{name}-{event}-{instance}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--exe-sha256", required=True, help="Previously verified frozen-main executable hash")
    parser.add_argument("--qt-bin", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    executable = identity(args.exe)
    require(executable["sha256"] == args.exe_sha256.lower(), "Frozen-main executable identity")
    args.out.mkdir(parents=True, exist_ok=False)
    reports = args.out / "reports"
    reports.mkdir()
    ui = args.out / "ui-reports"
    ui.mkdir()
    env = dict(os.environ)
    env["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + env["PATH"]
    manifest = {"schema": 1, "completed": False, "executable": executable, "cases": [],
                "uses_renderdoc": True, "full_mesh_parity": False}
    inputs, selections = {}, set()

    def save():
        (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    save()
    for case in CASES + ADDITIONAL_CASES:
        name = label(case)
        target = reports / name
        fresh = case in ADDITIONAL_CASES
        source = args.baseline / label((case[0], case[1], 0) if fresh else case)
        job = load(source / "job.json")
        require(job["gpa_event"] == case[1] and job["stage"] == "VSOut", "Baseline job event and stage")
        capture = Path(job["capture"])
        key = str(capture.resolve())
        if key not in inputs:
            inputs[key] = identity(capture)
        selected = (inputs[key]["sha256"], case[1], case[2], "VSOut")
        require(selected not in selections, "Duplicate comparison selection")
        selections.add(selected)
        record = {"case": name, "origin": "new_frozen_main_job" if fresh else "retained_baseline",
                  "capture": inputs[key], "gpa_event": case[1], "instance": case[2], "passed": False}
        manifest["cases"].append(record)
        save()
        if fresh:
            job.update(instance=case[2], out=target.resolve().as_posix())
            runtime = identity(Path(job["renderdoc"]))
            path = args.out / f"{name}-job.json"
            path.write_text(json.dumps(job, indent=2) + "\n", encoding="utf-8")
            command = [str(args.exe.resolve()), "--job", str(path.resolve())]
            record.update(command=command, runtime=runtime)
            save()
            try:
                run = subprocess.run(command, env=env, capture_output=True, timeout=180)
            except subprocess.TimeoutExpired as error:
                (args.out / f"{name}.log").write_bytes((error.stdout or b"") + (error.stderr or b""))
                record["error"] = "Baseline process exceeded its 180-second deadline"
                save()
                return 1
            (args.out / f"{name}.log").write_bytes(run.stdout + run.stderr)
            record["exit_code"] = run.returncode
            save()
            require(run.returncode == 0, "Baseline process failed; inspect retained output")
            # Preserve the precise job separately from any worker-generated metadata.
            (target / "job.json").write_text(json.dumps(job, indent=2) + "\n", encoding="utf-8")
            require(identity(Path(job["renderdoc"])) == runtime, "Analyzer library changed during execution")
        else:
            shutil.copytree(source, target)
            record["source"] = source.resolve().as_posix()
        result = load(target / "result.json")
        require(result["ok"] is True and result["gpa_event"] == case[1] and result["stage"] == "VSOut",
                "Baseline result selection")
        require(job["instance"] == case[2], "Baseline job instance")
        environment = result["replay_environment"]
        require(environment["gpu_vendor_name"] == "NVIDIA" and environment["degraded"] is False and
                environment["remote"] is False, "Actual baseline adapter")
        require((target / "post_vertices.bin").stat().st_size ==
                result["vertex_count"] * result["mesh"]["vertexByteStride"], "Vertex storage size")
        record.update(passed=True, vertex_count=result["vertex_count"], face_count=result["face_count"],
                      files=[identity(p) for p in sorted(target.iterdir()) if p.is_file()])
        shutil.copytree(target, ui / (name + "-native"))
        save()
        print(name, record["origin"], record["vertex_count"], flush=True)
    require(len(selections) == 10, "Complete distinct baseline collection")
    require(identity(args.exe) == executable, "Frozen-main executable changed")
    for original in inputs.values():
        require(identity(Path(original["path"])) == original, "Retained recapture changed")
    manifest["completed"] = True
    save()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
