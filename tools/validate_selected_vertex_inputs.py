"""Run selected VS input and native debug checks serially against saved full-draw observations.

The baseline reports come from FloraDxbcVertexInputProbe --compare-native before
selected-only acquisition. No baseline, golden, or capture is modified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

from validate_native_vertex_debug import validate


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def identity(path):
    path = Path(path)
    with path.open("rb") as stream:
        sha = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": sha}


def run(command, log):
    started = time.monotonic()
    with Path(log).open("wb") as stream:
        subprocess.run([str(x) for x in command], stdout=stream, stderr=subprocess.STDOUT,
                       timeout=300, check=True)
    return {"command": [str(x) for x in command], "seconds": time.monotonic() - started,
            "log": identity(log)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--captures", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--identity-index-override", action="store_true",
                        help="Exercise explicit index transport using the observed effective vertex index")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    cases = [("GF2_Exilium_2026_03_03__00_19_35.gpa_frame", 113),
             ("bf1_2026_01_21__16_53_05.gpa_frame", 876),
             ("helldivers2_2026_04_02__18_02_58.gpa_frame", 200)]
    summary = {"schema": 1, "complete_release_acceptance": False, "runs": [],
               "binaries": [identity(args.bin / name) for name in
                            ("FloraDxbcVertexInputProbe.exe", "FloraGPA.Cli.exe", "FloraGPA.Worker.exe",
                             "FloraNativeDebugSessionProbe.exe")]}
    for index, (capture, event) in enumerate(cases):
        for vendor in (4318, 32902, 0):
            stem = f"{index}-{vendor}"
            report_path = args.out / (stem + ".json")
            baseline_path = args.baseline / (stem + ".json")
            before = read(baseline_path)
            require(before["invocation"]["helper"]["backend"] == "dxbc-vertex-inputs/1",
                    "Reference was not produced by full-draw acquisition")
            probe = run([args.bin / "FloraDxbcVertexInputProbe.exe", args.captures / capture,
                         event, 0, 0, vendor, report_path, "--compare-native"],
                        args.out / (stem + "-probe.log"))
            after = read(report_path)
            for field in ("frame_sha256", "shader_sha256", "adapter_luid", "adapter_vendor", "adapter_device",
                          "input_registers", "final_registers", "execution_steps", "execution_complete"):
                require(after[field] == before[field], "Full-draw reference differs: " + stem + ":" + field)
            require(after["status"] == "ok" and after["execution_complete"] and
                    after["native_comparison"]["exact"], "Native shader comparison failed: " + stem)
            helper = after["invocation"]["helper"]
            require(helper["backend"] == "dxbc-selected-vertex-inputs/1" and helper["records"] == 1 and
                    helper["system_ids_source"] == "draw-relative-selection", "Wrong selected input provenance")
            job = args.out / (stem + "-job")
            command = [args.bin / "FloraGPA.Cli.exe", "analyze", args.captures / capture, "debug-vertex",
                       "--gpa-event", event, "--vertex", 0, "--instance", 0, "--out", job]
            command += ["--gpu-vendor", vendor] if vendor else ["--warp"]
            if args.identity_index_override:
                command += ["--index", before["invocation"]["vertex_index"]]
            cli = run(command, args.out / (stem + "-cli.log"))
            validation = args.out / (stem + "-validation.json")
            validate(job, report_path, args.bin / "FloraNativeDebugSessionProbe.exe", validation)
            summary["runs"].append({"case": stem, "probe": probe, "cli": cli,
                                    "baseline": identity(baseline_path), "report": identity(report_path),
                                    "validation": identity(validation),
                                    "full_draw_helper": before["invocation"]["helper"],
                                    "selected_helper": helper})
            (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
            print(stem + ": selected/full input bits, native outputs, Worker modules and session reload passed",
                  flush=True)


if __name__ == "__main__":
    main()
