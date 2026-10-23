"""Compare event-history texel snapshots with independent frozen-main prefix replays.

This bounded migration probe handles selected ordinary 2D, single-sample textures.
It does not establish fragment history, undefined-value equivalence or full parity.
GPU commands run serially. No external replay tool is loaded or launched.
"""
import argparse
import json
import mmap
import os
from pathlib import Path
import struct

from validate_native_analysis import digest, load, run


def texel_offset(capture, resource, x, y):
    with capture.open("rb") as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as raw:
        if raw[:4] != b"IGPA" or len(raw) < 260:
            raise ValueError("Invalid GPA header")
        count = struct.unpack_from("<I", raw, 12)[0]
        table = struct.unpack_from("<Q", raw, 244)[0]
        if table > len(raw) or count > (len(raw) - table) // 24:
            raise ValueError("Invalid GPA index")
        for index in range(count):
            identity, offset, size, flags, category, kind = struct.unpack_from("<QQIBBH", raw, table + index * 24)
            if identity != resource:
                continue
            if flags or category != 5 or kind != 0x85 or size != 68 or offset + size > len(raw):
                raise ValueError("Expected a complete 2D texture descriptor")
            width, height, mips, layers, fmt, samples, *_ = struct.unpack_from("<11I", raw, offset + 16)
            widths = {9: 8, 10: 8, 11: 8, 23: 4, 24: 4, 27: 4, 28: 4, 29: 4,
                      39: 4, 40: 4, 41: 4, 42: 4, 48: 2, 49: 2, 60: 1, 61: 1}
            if fmt not in widths or samples != 1 or not mips or not layers or x >= width or y >= height:
                raise ValueError("Selection is outside this probe's explicit format/layout scope")
            size = widths[fmt]
            return (y * width + x) * size, size
    raise ValueError("Selected resource is absent")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--baseline-cli", type=Path, required=True)
    parser.add_argument("--case", nargs=4, action="append", required=True,
                        metavar=("CAPTURE", "RESOURCE", "X", "Y"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--qt-bin", type=Path, required=True)
    parser.add_argument("--gpu-vendor", default="0x10de")
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    if args.out.exists() and any(args.out.iterdir()):
        parser.error("Output directory must be new or empty")
    args.out.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + environment.get("PATH", "")
    report = {"schema": 1, "scope": "selected_event_storage_prefix_comparison",
              "complete_release_acceptance": False, "cli_sha256": digest(args.cli),
              "baseline_cli_sha256": digest(args.baseline_cli), "cases": []}
    for index, (path, resource, x, y) in enumerate(args.case):
        capture = Path(path).resolve()
        directory = args.out / str(index)
        directory.mkdir()
        row = {"capture": str(capture), "resource": int(resource), "x": int(x), "y": int(y),
               "comparisons": [], "ok": False}
        report["cases"].append(row)
        try:
            offset, size = texel_offset(capture, int(resource), int(x), int(y))
            row["wall_seconds"] = run(
                [str(args.cli.resolve()), "analyze", str(capture), "history", "--history-scope", "events",
                 "--resource", resource, "--x", x, "--y", y, "--gpu-vendor", args.gpu_vendor,
                 "--timeout", str(args.timeout), "--out", str((directory / "native").resolve())],
                directory / "native.log", args.timeout + 30, environment)
            result = load(directory / "native/result.json")
            if not result["ok"] or result["context"]["frame_sha256"] != digest(capture):
                raise AssertionError("Native frame identity mismatch")
            if result["context"]["adapter"]["vendor"] != int(args.gpu_vendor, 0):
                raise AssertionError("Native analysis used another adapter")
            for name, expected in result["artifacts"].items():
                artifact = directory / "native" / name
                if artifact.stat().st_size != expected["bytes"] or digest(artifact) != expected["sha256"]:
                    raise AssertionError("Artifact hash mismatch")
            modules = [name.replace("\\", "/").lower() for name in result["execution"]["loaded_modules"]]
            if not modules or any("renderdoc" in name or "/intelswtools/gpa/" in name for name in modules):
                raise AssertionError("Missing or external-dependency module audit")
            history = result["data"]
            if history["fragment_history_available"] or history["replay_count"] != 1:
                raise AssertionError("Unexpected history provenance")
            records = history["records"]
            if not records:
                raise AssertionError("No candidate event snapshots")
            for record in records:
                if record["record_kind"] != "event_snapshot" or any(record[key] is not None for key in
                                                                       ("fragment", "shader_output", "failure_reasons")):
                    raise AssertionError("Event snapshot claims fragment evidence")
            row.update(frame_sha256=result["context"]["frame_sha256"], context=result["context"],
                       execution=result["execution"], records=len(records),
                       readback_count=history["readback_count"], readback_bytes=history["readback_bytes"])
            # Prefix replay uses the frozen executable and never executes the history collector.
            for position in sorted({0, len(records) // 2, len(records) - 1}):
                record = records[position]
                event = str(record["event"])
                value = record["after"]
                if not value["available"] or not value["exact_pixel_storage"]:
                    raise AssertionError("Selected checkpoint has no exact texel storage")
                target = directory / ("prefix-" + event)
                wall = run([str(args.baseline_cli.resolve()), "texture-storage", str(capture),
                            "--event", event, "--id", resource, "--gpu-vendor", args.gpu_vendor,
                            "--out", str(target.resolve())], directory / ("prefix-" + event + ".log"),
                           args.timeout, environment)
                raw = (target / "texture.bin").read_bytes()
                if offset + size > len(raw):
                    raise AssertionError("Prefix texture storage is incomplete")
                expected = raw[offset:offset + size].hex()
                row["comparisons"].append({"event": int(event), "native": value["bytes"],
                                           "prefix": expected, "equal": value["bytes"] == expected,
                                           "prefix_wall_seconds": wall, "prefix_storage_sha256": digest(target / "texture.bin")})
            row["ok"] = all(item["equal"] for item in row["comparisons"])
        except Exception as error:
            row["error"] = str(error)
        (args.out / "validation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    report["ok"] = all(row["ok"] for row in report["cases"])
    (args.out / "validation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(args.out / "validation.json")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
