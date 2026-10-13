"""Development-only, serial raw-resource replay comparisons; never an acceptance gate."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

import numpy as np


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def compare(first, other):
    if first.stat().st_size != other.stat().st_size:
        raise ValueError("Replay storage lengths differ")
    different = 0
    with first.open("rb") as left, other.open("rb") as right:
        while chunk := left.read(1024 * 1024):
            peer = right.read(len(chunk))
            if len(peer) != len(chunk):
                raise ValueError("Replay storage changed during comparison")
            different += int(np.count_nonzero(np.frombuffer(chunk, np.uint8) !=
                                             np.frombuffer(peer, np.uint8)))
    return different


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--frame", type=Path, required=True)
    parser.add_argument("--frame-sha256", required=True)
    parser.add_argument("--probes", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--qt-bin", type=Path)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=240)
    args = parser.parse_args()
    if not 2 <= args.repeats <= 10 or not 1 <= args.timeout <= 3600:
        parser.error("Expected 2..10 repeats and 1..3600 seconds per process")
    cli, frame, output = args.cli.resolve(), args.frame.resolve(), args.out.resolve()
    if not cli.is_file() or not frame.is_file():
        parser.error("CLI and frame must be existing files")
    frame_hash = digest(frame)
    if frame_hash != args.frame_sha256.lower():
        parser.error("Capture SHA256 mismatch")
    probes = json.loads(args.probes.read_text(encoding="utf-8-sig"))
    if not isinstance(probes, list) or not probes:
        parser.error("Expected a nonempty probe array")
    allowed = {"label", "command", "event", "resource", "before", "warp",
               "debug_device", "typed_format", "sample"}
    labels = set()
    for probe in probes:
        if not isinstance(probe, dict) or set(probe) - allowed:
            parser.error("Unknown probe fields")
        label = probe.get("label", "")
        if not isinstance(label, str) or not label or len(label) > 64 or any(
                c not in "abcdefghijklmnopqrstuvwxyz0123456789-_" for c in label):
            parser.error("Probe labels must be short lowercase ASCII names")
        if label in labels:
            parser.error("Duplicate probe label")
        labels.add(label)
        if probe.get("command") not in {"buffer", "texture", "texture-storage"}:
            parser.error("Unsupported probe command")
        for key in ("event", "resource"):
            if type(probe.get(key)) is not int or not 0 < probe[key] < 2**64:
                parser.error("Expected positive uint64 event/resource identities")
        for key in ("before", "warp", "debug_device"):
            if key in probe and type(probe[key]) is not bool:
                parser.error("Expected boolean probe switches")
        for key in ("typed_format", "sample"):
            if key in probe and (probe["command"] != "texture" or
                                 type(probe[key]) is not int or not 0 <= probe[key] < 2**32):
                parser.error("Texture format/sample must be uint32")
    # A new directory prevents accidental reuse of a prior successful output.
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    if args.qt_bin:
        env["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + env.get("PATH", "")
    result = dict(schema="FloraGPA repeatability diagnosis 1", completed=False,
                  acceptance_established=False, comparison="exact raw bytes",
                  frame=str(frame), frame_sha256=frame_hash, cli=str(cli),
                  cli_sha256=digest(cli), repeats=args.repeats, probes=probes, results=[])

    def save():
        (output / "results.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")

    save()
    try:
        for probe in probes:
            command = [str(cli), probe["command"], str(frame), "--event", str(probe["event"]),
                       "--id", str(probe["resource"])]
            command += ["--no-buffer-csv"] if probe["command"] == "buffer" else []
            command += ["--no-preview"] if probe["command"] == "texture" else []
            for key in ("before", "warp", "debug_device"):
                if probe.get(key, False):
                    command += ["--" + key.replace("_", "-")]
            for key in ("typed_format", "sample"):
                if key in probe:
                    command += ["--" + key.replace("_", "-"), str(probe[key])]
            row = dict(label=probe["label"], runs=[], different_bytes_vs_first=[])
            result["results"].append(row)
            paths = []
            for repeat in range(args.repeats):
                target = output / f"{probe['label']}-{repeat}"
                invocation = command + ["--out", str(target)]
                # No shell, and no concurrent GPU processes.
                with (output / f"{probe['label']}-{repeat}.log").open("wb") as log:
                    run = subprocess.run(invocation, env=env, stdout=log, stderr=subprocess.STDOUT,
                                         timeout=args.timeout, check=False)
                if run.returncode:
                    raise RuntimeError(f"{probe['label']} replay {repeat} exited {run.returncode}")
                report = json.loads((target / "report.json").read_text(encoding="utf-8"))
                if report.get("completed") is not True or report.get("reference_pixels_used") is not False:
                    raise RuntimeError("Replay did not establish execution without reference pixels")
                raw = target / ("buffer.bin" if probe["command"] == "buffer" else "texture.bin")
                if not raw.is_file() or not raw.stat().st_size:
                    raise RuntimeError("Replay storage missing or empty")
                paths.append(raw)
                row["runs"].append(dict(path=str(raw.relative_to(output)), bytes=raw.stat().st_size,
                                        sha256=digest(raw), report_sha256=digest(target / "report.json"),
                                        adapter=report.get("adapter"), command=invocation))
                if repeat:
                    row["different_bytes_vs_first"].append(compare(paths[0], raw))
                save()
            print(json.dumps({"label": row["label"], "different_bytes_vs_first":
                              row["different_bytes_vs_first"]}), flush=True)
        if digest(frame) != frame_hash:
            raise RuntimeError("Capture changed during diagnosis")
        result["completed"] = True
        save()
    except Exception as error:
        result["error"] = str(error)
        save()
        raise


if __name__ == "__main__":
    main()
