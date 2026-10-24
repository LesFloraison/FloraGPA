"""Validate retained debugger research evidence and write a compact, non-parity ledger."""

import argparse
import hashlib
import json
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--static", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]

    def identity(path):
        path = path.resolve()
        try:
            name = path.relative_to(repo).as_posix()
        except ValueError:
            name = path.as_posix()
        return dict(path=name, sha256=digest(path))

    manifest_path = args.root / "verified-final/runs.json"
    manifest = read(manifest_path)
    require(manifest["completed"] and len(manifest["runs"]) == 9, "Incomplete runtime manifest")
    for source in manifest["sources"]:
        path = Path(source["path"])
        if not path.is_absolute():
            path = repo / path
        require(digest(path) == source["sha256"], f"Runtime source changed: {path}")
    ledger = dict(schema="FloraGPA debugger hardware watch evidence 1", date="2026-10-24",
                  engine_recovered=False, shader_debug_parity=False,
                  scope="Constructor-to-destruction field-write events for six synchronous probe lifecycles; "
                        "no claim about other initialization routes or valid shader traces",
                  runtime_manifest=identity(manifest_path), sources=[], calibrations=[], runs=[],
                  static_binaries=read(args.static / "binaries.json"), static_exports=[],
                  retained_failures=[], superseded=[])
    for relative in ["tools/native/probe_original_debugger_watch.cpp",
                     "tools/run_original_debugger_watch.py", "tools/probe_original_debugger_host.py",
                     "tools/scan_gpa_debugger_modules.py", "tools/summarize_debugger_watch.py",
                     "tools/ghidra/Invoke-DebuggerWatchResearch.ps1",
                     "tools/ghidra/ExportSymbolEvidence.java", "tools/ghidra/ExportFunctionEvidence.java",
                     "tools/ghidra/ExportAddressEvidence.java"]:
        ledger["sources"].append(identity(repo / relative))
    for run in manifest["runs"]:
        require(run["exit_code"] == 0 and not run["timed_out"], f"Failed run: {run['name']}")
        name = run["name"]
        calibration = name.startswith("calibration-")
        path = args.root / "verified-final" / (name + ".json" if calibration else name + "/watch.json")
        watch = read(path)
        require(watch["completed"] and watch["exit_code"] == 0 and not watch["engine_recovered"] and
                not watch["shader_debug_parity"], f"Invalid watch result: {name}")
        require(watch["constructor_hits"] == 1 and watch["destructor_hits"] == 1 and
                watch["request_hits"] == 2, f"Incomplete lifecycle: {name}")
        writes = [event for event in watch["events"] if event["hits"] & 4]
        require(len(writes) == watch["field_writes"], f"Write count mismatch: {name}")
        row = dict(name=name, report=identity(path), elapsed_ms=watch["elapsed_ms"],
                   constructor_hits=watch["constructor_hits"], destructor_hits=watch["destructor_hits"],
                   field_writes=watch["field_writes"], request_hits=watch["request_hits"],
                   nonzero_live_field_observations=watch["nonzero_live_field_writes"])
        if calibration:
            writer_threads = {event["thread_id"] for event in writes}
            require(len(writes) == 36 and len(writer_threads) == 3 and
                    watch["nonzero_live_field_writes"] > 0, f"Concurrent calibration failed: {name}")
            row["writer_thread_count"] = len(writer_threads)
            ledger["calibrations"].append(row)
            continue
        dispatch_path = path.with_name("dispatch.json")
        dispatch = read(dispatch_path)
        require(digest(path) == run["watch"]["sha256"] and
                digest(dispatch_path) == run["dispatch"]["sha256"], f"Evidence changed: {name}")
        require(dispatch["completed"] and dispatch["player_unchanged"] and dispatch["capture_unchanged"]
                and not dispatch["errors"] and not dispatch["observer_errors"], f"Dispatch failed: {name}")
        require(len(writes) == 2 and watch["nonzero_live_field_writes"] == 0,
                f"Lifecycle findings changed: {name}")
        require(writes[0]["lifecycle_phase"] == "constructing" and writes[0]["written_engine"] == "0x0"
                and writes[1]["lifecycle_phase"] == "destroying", f"Unexpected field-write phases: {name}")
        requests = [event for event in watch["events"] if event["hits"] & 8]
        require(all(event["request_engine"] == "0x0" for event in requests), f"Non-null request: {name}")
        require(len(dispatch["requests"]) == 2 and
                all(request["status"] == 0 and not request["result_callbacks"] and
                    request["observer_restored"] for request in dispatch["requests"]),
                f"Unexpected request outcome: {name}")
        row.update(dispatch=identity(dispatch_path), capture_sha256=dispatch["capture_sha256"],
                   player_sha256=watch["player_sha256"], event=dispatch["event"],
                   settings=dispatch["settings"], adapter_selection=dispatch["adapter_selection"],
                   raw_selection=dispatch["requests"][1]["execution"][0]["selection"],
                   result_callback_count=0,
                   field_observations=[dict(phase=event["lifecycle_phase"],
                                            value=event["written_engine"],
                                            instruction_module=event["instruction_module"])
                                       for event in writes])
        ledger["runs"].append(row)
    for entry in read(args.static / "evidence-sha256.json"):
        path = args.static / entry["name"]
        require(digest(path) == entry["sha256"], f"Static evidence changed: {path}")
        if path.suffix == ".txt":
            lines = path.read_text(encoding="utf-8-sig").splitlines()
            require(lines[-1].startswith("COMPLETE "), f"Incomplete export: {path}")
            ledger["static_exports"].append(dict(**identity(path), completion=lines[-1]))
    require(len(ledger["static_exports"]) == 11, "Missing static exports")
    failures = [("gf2-controlled/watch.json", "Teardown heap-fill pointer dereference was not optional"),
                ("self-test-final.json", "Identical fixture lifecycle functions folded by linker ICF"),
                ("self-test-concurrent-1.json", "Reconfiguring other threads interfered with queued traps")]
    for name, reason in failures:
        path = args.root / name
        result = read(path)
        require(not result["completed"], f"Expected retained failure: {path}")
        ledger["retained_failures"].append(dict(**identity(path), reason=reason, error=result.get("error")))
    for name in ["gf2-controlled-final", "gf2-default", "bf1-controlled", "bf1-default",
                 "helldivers-controlled", "helldivers-default"]:
        ledger["superseded"].append(dict(**identity(args.root / name / "watch.json"),
                                        reason="Predates concurrent calibration and thread-context fix"))
    ledger["build_log"] = identity(args.root / "build-seventh.txt")
    ledger["capture_boundaries"] = identity(args.root / "capture-boundaries.txt")
    ledger["module_candidates"] = identity(args.root / "module-candidates.json")
    negative_path = args.root / "summary-negative.json"
    negative = read(negative_path)
    require(negative["rejected"] and negative["exit_code"] != 0 and
            "Static evidence changed:" in negative["error"], "Missing evidence-integrity rejection")
    ledger["integrity_negative_check"] = identity(negative_path)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(ledger, indent=2) + "\n", encoding="utf-8")
    print("Validated 3 concurrent calibrations, 6 GPA lifecycles and 11 static exports; engine remains open")


if __name__ == "__main__":
    main()
