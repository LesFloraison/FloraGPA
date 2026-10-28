"""Migration-only baseline observation; never an independent release requirement.

Run frozen main recapture/debug and current native jobs sequentially against the
saved ClassCapture fixtures. Preserve failures and actual replay environments.
No baseline failure exempts the native executor's independent arithmetic tests.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("baseline", "native", "frames", "qt", "renderdoc", "out"):
        parser.add_argument("--" + key, required=True, type=Path)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(exist_ok=False, parents=True)
    env = os.environ.copy()
    env["PATH"] = str(args.qt.resolve()) + os.pathsep + env.get("PATH", "")
    env["QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES"] = "1"
    baseline = args.baseline.resolve() / "FloraGPA.Cli.exe"
    native = args.native.resolve() / "FloraGPA.Cli.exe"
    summary = {"schema": 1, "baseline": "3c71b0af1cd1fce395c05b1f6ddc6a41d3e13b28",
               "full_parity": False, "files": [], "cases": []}
    for path in (baseline, args.baseline.resolve() / "FloraGPA.Rdc.exe", native,
                 args.native.resolve() / "FloraGPA.Worker.exe", args.renderdoc.resolve()):
        summary["files"].append({"path": str(path), "sha256": digest(path), "bytes": path.stat().st_size})

    def run(name, command):
        command = list(map(str, command))
        start = time.monotonic()
        process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        timed_out = False
        try:
            stdout, stderr = process.communicate(timeout=90)
        except subprocess.TimeoutExpired:
            timed_out = True
            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                           capture_output=True, timeout=15, check=False)
            stdout, stderr = process.communicate(timeout=10)
        (args.out / (name + ".stdout")).write_bytes(stdout)
        (args.out / (name + ".stderr")).write_bytes(stderr)
        return {"command": command, "exit_code": process.returncode, "timeout": timed_out,
                "seconds": time.monotonic() - start}

    def save():
        (args.out / "matrix.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    for vendor in (0, 4318, 32902):
        for created in (0, 1):
            label = f"{vendor}-{created}"
            frame = args.frames.resolve() / f"capture-{label}.gpa_frame"
            case = {"vendor_request": vendor, "created": bool(created), "frame": str(frame),
                    "frame_sha256": digest(frame), "gpa_event": 100, "thread": [0, 0, 0]}
            summary["cases"].append(case)
            selection = ["--gpu-vendor", str(vendor)] if vendor else ["--warp"]
            recapture = args.out / (label + "-recapture")
            case["recapture"] = run(label + "-recapture", [baseline, "replay", frame, *selection,
                                     "--renderdoc", args.renderdoc.resolve(), "--out", recapture])
            capture_report = load(recapture / "report.json")
            if capture_report:
                case["capture_adapter"] = capture_report.get("adapter")
                case["capture_completed"] = capture_report.get("completed")
            if case["recapture"]["exit_code"] == 0 and capture_report and capture_report.get("rdc_capture"):
                old = args.out / (label + "-baseline-debug")
                case["baseline_debug"] = run(label + "-baseline-debug", [baseline, "rdc-analyze",
                    capture_report["rdc_capture"], "debug-thread", "--gpa-event", "100",
                    "--group", "0", "0", "0", "--thread", "0", "0", "0", "--timeout", "60",
                    "--renderdoc", args.renderdoc.resolve(), "--out", old])
                result = load(old / "result.json")
                case["baseline_result"] = None if result is None else {
                    key: result.get(key) for key in ("ok", "error", "step_count", "replay_environment",
                                                    "gpa_event", "eid")}
            new = args.out / (label + "-native-debug")
            case["native_debug"] = run(label + "-native-debug", [native, "analyze", frame,
                "debug-thread", "--gpa-event", "100", "--group", "0", "0", "0",
                "--thread", "0", "0", "0", *selection, "--timeout", "60", "--out", new])
            result = load(new / "result.json")
            case["native_result"] = None if result is None else {
                "ok": result.get("ok"), "error": result.get("error"),
                "adapter": result.get("context", {}).get("adapter"), "data": result.get("data")}
            save()
            print(label, "baseline", case.get("baseline_result"),
                  "native", None if result is None else result.get("ok"), flush=True)
    summary["observation_complete"] = all(
        c.get("capture_completed") is True and c.get("baseline_result") is not None and
        c.get("native_result") is not None and not any(c[k]["timeout"] for k in
        ("recapture", "baseline_debug", "native_debug")) for c in summary["cases"])
    summary["baseline_successes"] = sum((c.get("baseline_result") or {}).get("ok") is True
                                        for c in summary["cases"])
    summary["native_successes"] = sum((c.get("native_result") or {}).get("ok") is True
                                      for c in summary["cases"])
    save()
    # Successful observation is not passing baseline/native parity.
    return 0 if summary["observation_complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
