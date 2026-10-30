"""Run coverage normalization probes serially with bounded processes and immutable outputs."""
import argparse
import itertools
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binaries", "qt-bin", "captures", "baseline", "out"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    root = args.out.resolve()
    binaries = args.binaries.resolve()
    names = ("FloraDxbcPixelCoverageTests", "FloraNativePixelOrdinalReplayTests",
             "FloraNativePixelOrdinalTests", "FloraNativePixelCompletionProbe")
    for name in names:
        if not (binaries / (name + ".exe")).is_file():
            raise ValueError("Missing coverage probe executable: " + name)
    if not args.baseline.is_dir() or not (args.qt_bin / "Qt6Core.dll").is_file():
        raise ValueError("Missing baseline or Qt runtime")
    root.mkdir(parents=True, exist_ok=False)
    (root / "binaries").mkdir()
    for name in names:
        shutil.copy2(binaries / (name + ".exe"), root / "binaries" / (name + ".exe"))
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith(
        ("FLORA_PIXEL_ORDINAL_", "FLORA_PIXEL_COVERAGE_"))}
    env["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + env.get("PATH", "")
    rows = []

    def run(label, command, variables=None, timeout=600):
        command = list(map(str, command))
        variables = variables or {}
        start = time.monotonic()
        with (root / (label + "-console.txt")).open("w", encoding="utf-8") as log:
            try:
                code = subprocess.run(command, env=env | variables, stdout=log, stderr=subprocess.STDOUT,
                                      timeout=timeout).returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
        rows.append({"label": label, "command": command, "variables": variables, "exit_code": code,
                     "elapsed_seconds": time.monotonic() - start})
        (root / "exits.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
        print(label, code, flush=True)

    for name, variable, folder in ((names[0], "FLORA_PIXEL_COVERAGE_EVIDENCE", "semantics"),
                                    (names[1], "FLORA_PIXEL_ORDINAL_REPLAY_EVIDENCE", "native-final")):
        run(name, [binaries / (name + ".exe"), "-o", str(root / (name + ".txt")) + ",txt"],
            {variable: str(root / folder)})
    cases = [f"extraction:{vendor}-{layout}-1-3-8" for vendor, layout in itertools.product((0, 4318, 32902), range(3))]
    for label, wide, normalize in (("d24s8", 0, 0), ("d32s8", 1, 0), ("normalized-final", 0, 1)):
        variables = {"FLORA_PIXEL_ORDINAL_" + k: str(v) for k, v in (
            ("EVIDENCE", root / label), ("EXTENT", 1), ("FLOAT", 1), ("RASTER_MSAA", 0),
            ("WIDE_STENCIL", wide), ("NORMALIZE_COVERAGE", normalize))}
        run(label, [binaries / (names[2] + ".exe"), *cases, "-o", str(root / (label + ".txt")) + ",txt"], variables)
    (root / "real-final").mkdir()
    for name, filename, event, x, y in (
            ("gf2", "GF2_Exilium_2026_03_03__00_19_35.gpa_frame", 113, 799, 480),
            ("bf1", "bf1_2026_01_21__16_53_05.gpa_frame", 2010, 751, 433),
            ("helldivers", "helldivers2_2026_04_02__18_02_58.gpa_frame", 200, 168, 183)):
        frame = args.captures.resolve() / filename
        if not frame.is_file():
            raise ValueError("Missing capture: " + str(frame))
        for vendor in (0, 4318, 32902):
            label = f"{name}-{vendor}"
            run(label, [binaries / (names[3] + ".exe"), frame, event, 0, x, y, 0, vendor,
                        root / "real-final" / label, "--ordinal", 0], timeout=180)
    run("verification", [sys.executable, Path(__file__).resolve().with_name("validate_pixel_coverage.py"),
                         "--root", root, "--semantics", root / "semantics", "--baseline", args.baseline.resolve(),
                         "--out", root / "verification.json"])
    return int(any(row["exit_code"] != 0 for row in rows))


if __name__ == "__main__":
    sys.exit(main())
