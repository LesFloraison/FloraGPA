"""Run serial, bounded hardware-watch calibration and original GPA lifecycle probes."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def identity(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return dict(path=path.as_posix(), sha256=digest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--watcher", type=Path, required=True)
    parser.add_argument("--capture-root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    child = Path(__file__).with_name("probe_original_debugger_host.py").resolve()
    report = dict(schema=1, scope="original_gpa_hardware_watch_validation", completed=False,
                  engine_recovered=False, shader_debug_parity=False,
                  sources=[identity(Path(__file__)), identity(child), identity(args.watcher)], runs=[])
    def save():
        (args.out / "runs.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    def run(name, command):
        with (args.out / (name + ".log")).open("w", encoding="utf-8") as log:
            try:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=130, creationflags=subprocess.CREATE_NO_WINDOW)
                status = result.returncode
            except subprocess.TimeoutExpired:
                status = None
        report["runs"].append(dict(name=name, command=command, exit_code=status, timed_out=status is None))
        save()
        if status != 0:
            raise RuntimeError(f"Hardware-watch run failed: {name}")
    save()
    try:
        for i in range(3):
            path = args.out / f"calibration-{i}.json"
            run(f"calibration-{i}", [str(args.watcher.resolve()), str(path.resolve()), "--self-test"])
            observation = json.loads(path.read_text(encoding="utf-8"))
            if not observation["completed"] or observation["field_writes"] != 36:
                raise RuntimeError("Concurrent calibration did not observe all fixture writes")
        captures = [("gf2", "GF2_Exilium_2026_03_03__00_19_35.gpa_frame", 113),
                    ("bf1", "bf1_2026_01_21__16_53_05.gpa_frame", 876),
                    ("helldivers", "helldivers2_2026_04_02__18_02_58.gpa_frame", 200)]
        for name, filename, event in captures:
            for original in [False, True]:
                case = name + ("-default" if original else "-controlled")
                directory = args.out / case
                directory.mkdir()
                command = [str(args.watcher.resolve()), str((directory / "watch.json").resolve()),
                           sys.executable, str(child), str((args.capture_root / filename).resolve()),
                           "--event", str(event), "--out", str(directory.resolve()), "--child"]
                if original:
                    command.append("--original-initialization")
                run(case, command)
                observation = json.loads((directory / "watch.json").read_text(encoding="utf-8"))
                dispatch = json.loads((directory / "dispatch.json").read_text(encoding="utf-8"))
                if not (observation["completed"] and dispatch["completed"] and
                        dispatch["player_unchanged"] and dispatch["capture_unchanged"]):
                    raise RuntimeError(f"Incomplete lifecycle evidence: {case}")
                report["runs"][-1].update(watch=identity(directory / "watch.json"),
                                         dispatch=identity(directory / "dispatch.json"))
                save()
                print(case, "recorded", flush=True)
        report["completed"] = True
        save()
    except Exception as error:
        report["error"] = str(error)
        save()
        raise


if __name__ == "__main__":
    main()
