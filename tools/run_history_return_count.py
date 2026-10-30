"""Exercise private-history return counts through isolated native analysis jobs."""
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
    root, binaries = args.out.resolve(), args.binaries.resolve()
    suites = ("FloraNativeDrawHistoryJobsTests", "FloraNativeAnalysisTests", "FloraNativeDebugSessionTests")
    names = (*suites, "FloraGPA.Cli", "FloraGPA.Worker")
    frames = (("gf2", "GF2_Exilium_2026_03_03__00_19_35.gpa_frame", 113, 80, 799, 480),
              ("bf1", "bf1_2026_01_21__16_53_05.gpa_frame", 2010, 954, 751, 433),
              ("helldivers", "helldivers2_2026_04_02__18_02_58.gpa_frame", 200, 99, 168, 183))
    inputs = [binaries / (n + ".exe") for n in names]
    inputs += [args.captures.resolve() / row[1] for row in frames]
    identities = []
    for path in inputs:
        with path.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        identities.append({"path": str(path), "bytes": path.stat().st_size, "sha256": digest})
    root.mkdir(parents=True, exist_ok=False)
    (root / "binaries").mkdir()
    for name in names:
        shutil.copy2(binaries / (name + ".exe"), root / "binaries" / (name + ".exe"))
    (root / "inputs.json").write_text(json.dumps(identities, indent=2) + "\n", encoding="utf-8")
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith(("FLORA_PIXEL_", "FLORA_DRAW_HISTORY_"))}
    env["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + env.get("PATH", "")
    env["QT_QPA_PLATFORM"] = "offscreen"
    exits = []

    def run(label, command, extra=None):
        command = list(map(str, command))
        start = time.monotonic()
        with (root / (label + "-console.txt")).open("w", encoding="utf-8") as output:
            try:
                code = subprocess.run(command, env=env | (extra or {}), stdout=output,
                                      stderr=subprocess.STDOUT, timeout=900).returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
        exits.append({"label": label, "command": command, "exit_code": code,
                      "elapsed_seconds": time.monotonic() - start})
        (root / "exits.json").write_text(json.dumps(exits, indent=2) + "\n", encoding="utf-8")
        print(label, code, flush=True)
        if code == "timeout":
            raise RuntimeError("Stopped after bounded process timeout")

    for suite in suites:
        run(suite, [binaries / (suite + ".exe"), "-o", str(root / (suite + ".txt")) + ",txt"],
            {"FLORA_DRAW_HISTORY_JOBS_EVIDENCE": str(root / "owned")} if suite == suites[0] else {})
    for label, name, event, resource, x, y in frames:
        for vendor in (0, 4318, 32902):
            run(f"{label}-{vendor}", [binaries / "FloraGPA.Cli.exe", "analyze", args.captures.resolve() / name,
                "history", "--history-scope", "private-draw", "--gpa-event", event, "--resource", resource,
                "--x", x, "--y", y, "--return-count", "--debug-device", "--timeout", 120,
                *( ["--gpu-vendor", vendor] if vendor else ["--warp"] ), "--out", root / "real" / f"{label}-{vendor}"])
    return int(any(row["exit_code"] != 0 for row in exits))


if __name__ == "__main__":
    raise SystemExit(main())
