"""Run stencil-reference research serially and retain every process failure."""
import argparse
import hashlib
import itertools
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binaries", "qt-bin", "captures", "ghidra", "sdk-header", "out"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    root = args.out.resolve()
    binaries = args.binaries.resolve()
    names = ("FloraNativePixelOrdinalReplayTests", "FloraNativePixelGeometryReplayTests",
             "FloraNativePixelPresenceTests", "FloraNativePixelOrdinalTests", "FloraNativePixelCompletionProbe")
    frames = (("gf2", "GF2_Exilium_2026_03_03__00_19_35.gpa_frame", 113, 799, 480),
              ("bf1", "bf1_2026_01_21__16_53_05.gpa_frame", 2010, 751, 433),
              ("helldivers", "helldivers2_2026_04_02__18_02_58.gpa_frame", 200, 168, 183))
    inputs = [binaries / (n + ".exe") for n in names]
    inputs += [args.captures.resolve() / row[1] for row in frames]
    inputs += [args.qt_bin.resolve() / "Qt6Core.dll", args.sdk_header.resolve()]
    for path in inputs:
        if not path.is_file():
            raise ValueError(f"Missing input: {path}")
    if not args.ghidra.is_dir():
        raise ValueError("Missing previous Ghidra counter-state export directory")
    root.mkdir(parents=True, exist_ok=False)
    identities = []
    for path in inputs:
        with path.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        identities.append({"path": str(path), "bytes": path.stat().st_size, "sha256": digest})
    (root / "inputs.json").write_text(json.dumps(identities, indent=2) + "\n", encoding="utf-8")
    (root / "binaries").mkdir()
    for name in names:
        shutil.copy2(binaries / (name + ".exe"), root / "binaries" / (name + ".exe"))
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("FLORA_PIXEL_ORDINAL_")}
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

    for name in names[:4]:
        variables = {}
        if name == names[0]:
            variables["FLORA_PIXEL_ORDINAL_REPLAY_EVIDENCE"] = str(root / "final")
        elif name == names[3]:
            variables["FLORA_PIXEL_ORDINAL_EVIDENCE"] = str(root / "default-probe")
        run(name, [binaries / (name + ".exe"), "-o", str(root / (name + ".txt")) + ",txt"], variables)
    for label, vendors, extent, floating, raster, mip in (
            ("independent-float", (0, 4318, 32902), 1, 1, 0, 0),
            ("independent-raster-msaa", (32902,), 1, 1, 1, 0),
            ("independent-float-four", (32902,), 4, 1, 1, 0),
            ("independent-uint-one", (32902,), 1, 0, 1, 0),
            ("independent-stencil-mip", (32902,), 1, 1, 0, 2)):
        variables = {"FLORA_PIXEL_ORDINAL_" + key: str(value) for key, value in (
            ("EVIDENCE", root / label), ("EXTENT", extent), ("FLOAT", floating),
            ("RASTER_MSAA", raster), ("STENCIL_MIP", mip))}
        cases = [f"extraction:{vendor}-{layout}-1-3-8" for vendor, layout in itertools.product(vendors, range(3))]
        run(label, [binaries / (names[3] + ".exe"), *cases, "-o", str(root / (label + ".txt")) + ",txt"], variables)
    (root / "real").mkdir()
    for name, frame, event, x, y in frames:
        for vendor in (0, 4318, 32902):
            label = f"{name}-{vendor}"
            run(label, [binaries / (names[4] + ".exe"), args.captures.resolve() / frame,
                        event, 0, x, y, 0, vendor, root / "real" / label, "--ordinal", 0], timeout=180)
    tools = Path(__file__).resolve().parent
    run("verification", [sys.executable, tools / "validate_pixel_ordinal_replay.py", "--root", root,
                         "--out", root / "final-verification.json"])
    run("default-verification", [sys.executable, tools / "validate_pixel_ordinals.py", "--root",
                                 root / "default-probe", "--ghidra", args.ghidra.resolve(),
                                 "--sdk-header", args.sdk_header.resolve(), "--out",
                                 root / "default-verification.json"])
    return int(any(row["exit_code"] != 0 for row in rows))


if __name__ == "__main__":
    sys.exit(main())
