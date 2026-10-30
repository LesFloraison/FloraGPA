"""Run the owned MRT-binding ordinal experiment serially, retaining strict failures."""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "qt-bin", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    root = args.out.resolve()
    binary = args.binary.resolve()
    qt = args.qt_bin.resolve()
    inputs = [binary, qt / "Qt6Core.dll", qt / "Qt6Test.dll"]
    for path in inputs:
        if not path.is_file():
            raise ValueError(f"Missing input: {path}")
    root.mkdir(parents=True, exist_ok=False)
    identities = [{"path": str(p), "sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
                   "bytes": p.stat().st_size} for p in inputs]
    (root / "inputs.json").write_text(json.dumps(identities, indent=2) + "\n", encoding="utf-8")
    shutil.copy2(binary, root / binary.name)
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("FLORA_PIXEL_ORDINAL_")}
    env["PATH"] = str(qt) + os.pathsep + env.get("PATH", "")
    exits = []
    cases = [f"extraction:{v}-{layout}-{samples}-0-8"
             for v, layout, samples in itertools.product((0, 4318, 32902), range(3), (1, 4))]
    for mask in (255, 128, 192, 240, None):
        label = "default" if mask is None else f"mask-{mask}"
        variables = {"FLORA_PIXEL_ORDINAL_EVIDENCE": str(root / label)}
        if mask is not None:
            variables.update(FLORA_PIXEL_ORDINAL_TARGET_MASK=str(mask), FLORA_PIXEL_ORDINAL_REPETITIONS="4")
        command = [str(binary), *([] if mask is None else cases), "-o", str(root / (label + ".txt")) + ",txt"]
        start = time.monotonic()
        with (root / (label + "-console.txt")).open("w", encoding="utf-8") as output:
            try:
                code = subprocess.run(command, env=env | variables, stdout=output,
                                      stderr=subprocess.STDOUT, timeout=180).returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
        exits.append({"label": label, "command": command, "environment": variables,
                      "exit_code": code, "elapsed_seconds": time.monotonic() - start})
        (root / "exits.json").write_text(json.dumps(exits, indent=2) + "\n", encoding="utf-8")
        print(label, code, flush=True)
        if code == "timeout":
            break
    # A completed research run is not a semantic pass. Keep the probe's failure status.
    return int(any(row["exit_code"] != 0 for row in exits))


if __name__ == "__main__":
    raise SystemExit(main())
