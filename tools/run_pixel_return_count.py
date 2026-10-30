"""Exercise native return counting and related regressions on serial GPU processes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binaries", "qt-bin", "captures", "out"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    root = args.out.resolve()
    binaries = args.binaries.resolve()
    names = ("FloraNativePixelReturnCountTests", "FloraNativePixelOrdinalReplayTests",
             "FloraNativePixelPresenceTests", "FloraDxbcPixelCompletionTests", "FloraNativePixelGeometryReplayTests")
    probe = "FloraNativePixelCompletionProbe"
    frames = (("gf2", "GF2_Exilium_2026_03_03__00_19_35.gpa_frame", 113, 799, 480),
              ("bf1", "bf1_2026_01_21__16_53_05.gpa_frame", 2010, 751, 433),
              ("helldivers", "helldivers2_2026_04_02__18_02_58.gpa_frame", 200, 168, 183))
    inputs = [binaries / (name + ".exe") for name in (*names, probe)]
    inputs += [args.captures.resolve() / row[1] for row in frames]
    inputs += [args.qt_bin.resolve() / "Qt6Core.dll", args.qt_bin.resolve() / "Qt6Test.dll"]
    for path in inputs:
        if not path.is_file():
            raise ValueError(f"Missing input: {path}")
    root.mkdir(parents=True, exist_ok=False)
    identities = []
    for path in inputs:
        with path.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        identities.append({"path": str(path), "bytes": path.stat().st_size, "sha256": digest})
    (root / "inputs.json").write_text(json.dumps(identities, indent=2) + "\n", encoding="utf-8")
    (root / "binaries").mkdir()
    for name in (*names, probe):
        shutil.copy2(binaries / (name + ".exe"), root / "binaries" / (name + ".exe"))
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("FLORA_PIXEL_")}
    env["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + env.get("PATH", "")
    exits = []

    def run(label, command, variables=None):
        command = list(map(str, command))
        start = time.monotonic()
        with (root / (label + "-console.txt")).open("w", encoding="utf-8") as output:
            try:
                code = subprocess.run(command, env=env | (variables or {}), stdout=output,
                                      stderr=subprocess.STDOUT, timeout=300).returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
        exits.append({"label": label, "command": command, "variables": variables or {},
                      "exit_code": code, "elapsed_seconds": time.monotonic() - start})
        (root / "exits.json").write_text(json.dumps(exits, indent=2) + "\n", encoding="utf-8")
        print(label, code, flush=True)
        if code == "timeout":
            raise RuntimeError("Stopped after a bounded GPU process timed out")

    for name in names:
        variables = {}
        if name == names[0]:
            variables["FLORA_PIXEL_RETURN_COUNT_EVIDENCE"] = str(root / "native")
        if name == names[1]:
            variables["FLORA_PIXEL_ORDINAL_REPLAY_EVIDENCE"] = str(root / "ordinal")
        run(name, [binaries / (name + ".exe"), "-o", str(root / (name + ".txt")) + ",txt"], variables)
    for label, frame, event, x, y in frames:
        for vendor in (0, 4318, 32902):
            run(f"{label}-{vendor}", [binaries / (probe + ".exe"), args.captures.resolve() / frame,
                event, 0, x, y, 0, vendor, root / "real" / f"{label}-{vendor}", "--return-count"])
    return int(any(row["exit_code"] != 0 for row in exits))


if __name__ == "__main__":
    raise SystemExit(main())
