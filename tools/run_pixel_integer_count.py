"""Retain integer-output count observations from one bounded serial GPU suite."""
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
    for name in ("binaries", "qt-bin", "out"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    root = args.out.resolve()
    exe = args.binaries.resolve() / "FloraNativePixelIntegerCountTests.exe"
    inputs = [exe] + [args.qt_bin.resolve() / (name + ".dll") for name in ("Qt6Core", "Qt6Test")]
    identities = []
    for path in inputs:
        with path.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        identities.append({"path": str(path), "bytes": path.stat().st_size, "sha256": digest})
    root.mkdir(parents=True, exist_ok=False)
    (root / "inputs.json").write_text(json.dumps(identities, indent=2) + "\n", encoding="utf-8")
    shutil.copy2(exe, root / exe.name)
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("FLORA_PIXEL_")}
    env["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + env.get("PATH", "")
    env["FLORA_PIXEL_INTEGER_COUNT_EVIDENCE"] = str(root / "native")
    command = [str(root / exe.name), "-o", str(root / "tests.txt") + ",txt"]
    start = time.monotonic()
    with (root / "console.txt").open("w", encoding="utf-8") as output:
        try:
            code = subprocess.run(command, env=env, stdout=output, stderr=subprocess.STDOUT,
                                  timeout=900).returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
    result = {"command": command, "exit_code": code, "elapsed_seconds": time.monotonic() - start}
    (root / "exit.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))
    return int(code != 0)


if __name__ == "__main__":
    raise SystemExit(main())
