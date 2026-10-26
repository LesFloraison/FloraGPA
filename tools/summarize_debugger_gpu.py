"""Validate explicit-adapter GPA debugger lifecycle evidence without claiming parity."""
import argparse
import copy
import itertools
import json
from pathlib import Path

from probe_original_debugger_host import PLAYER_SHA256, digest


def require(value, message):
    if not value:
        raise ValueError(message)


def validate_dispatch(report):
    require(report["completed"] and report["closed"] and report["player_unchanged"] and
            report["capture_unchanged"] and report["player_sha256"] == PLAYER_SHA256,
            "Incomplete original lifecycle")
    require(not report["errors"] and not report["observer_errors"], "Original dispatch error")
    selected, actual = report["selected_gpu"], report["replay_adapter"]
    require(selected in report["gpu_enumeration"] and selected["gpu_id"][0] == 1,
            "Selection not in original DXGI enumeration")
    require(actual["vendor"] == selected["vendor"] == report["requested_vendor"] and
            actual["device"] == selected["device"] and
            actual["luid"] == (selected["gpu_id"][1] | (selected["gpu_id"][2] << 32)),
            "Actual replay adapter differs")
    require(actual == report["replay_adapter_after"], "Replay adapter changed")
    require(len(report["playbacks"]) == 2 and all(row["status"] == 0 and row["error"] == "0x0"
            for row in report["playbacks"]), "Replay failed")
    requests = report["requests"]
    require(len(requests) == 2 and all(row["status"] == 0 and row["error"] == "0x0" and
            row["observer_restored"] and not row["result_callbacks"] for row in requests),
            "Request outcome changed")
    require(len(requests[1]["execution"]) == 1, "Missing observed request")
    request = requests[1]["execution"][0]
    require(request["engine_before"] == request["engine_after"] == "0x0" and
            request["host_matches_global"] and request["request_vtable_rva"] == "0x183198" and
            request["selection"] == [0, report["event"], 0, 0, 0, 0, 0, 0],
            "Debugger request findings changed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--additional", type=Path, help="Supplemental runner manifest after a failed bounded run")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    read = lambda p: json.loads(p.read_text(encoding="utf-8-sig"))
    def identity(p):
        p = p.resolve()
        return dict(path=p.relative_to(repo).as_posix(), bytes=p.stat().st_size, sha256=digest(p))
    manifests = [args.root / "watched/runs.json"] + ([args.additional] if args.additional else [])
    rows = []
    for path in manifests:
        manifest = read(path)
        for source in manifest["sources"]:
            require(digest(Path(source["path"])) == source["sha256"], "Runtime source changed")
        rows.extend((path.parent, row) for row in manifest["runs"])
    require(len(rows) == 15, "Incomplete watch matrix")
    runs, failures, calibrations, seen, controls = [], [], [], set(), {}
    for directory, row in rows:
        name = row["name"]
        calibration = name.startswith("calibration-")
        path = directory / (name + ".json" if calibration else name + "/watch.json")
        watch = read(path)
        if row["exit_code"] != 0 or row["timed_out"]:
            require(not calibration and not watch["completed"] and
                    watch["error"] == "Hardware watch timed out" and watch["timeout_ms"] == 120000,
                    "Unexpected failed-run classification")
            dispatch_path = path.with_name("dispatch.json")
            dispatch = read(dispatch_path)
            require(not dispatch["completed"] and not dispatch["errors"] and
                    not dispatch["observer_errors"], "Unexpected partial dispatch failure")
            actual = dispatch.get("replay_adapter")
            selected = dispatch["selected_gpu"]
            if actual:
                require(actual["vendor"] == selected["vendor"] == dispatch["requested_vendor"] and
                        actual["device"] == selected["device"] and
                        actual["luid"] == (selected["gpu_id"][1] | selected["gpu_id"][2] << 32),
                        "Partial run selected another adapter")
            require(digest(Path(dispatch["capture"])) == dispatch["capture_sha256"],
                    "Capture changed during incomplete run")
            request_events = [e for e in watch["events"] if e["hits"] & 8]
            require(len(request_events) == len(dispatch["requests"]) and
                    all(e["request_engine"] == "0x0" for e in request_events) and
                    all(r["status"] == 0 and r["observer_restored"] and not r["result_callbacks"]
                        for r in dispatch["requests"]), "Partial request observations changed")
            key = (dispatch["event"], dispatch["requested_vendor"], "no-first-playbacks" in dispatch["settings"])
            require(key not in seen, "Duplicate lifecycle")
            seen.add(key)
            failures.append(dict(name=name, watch=identity(path), dispatch=identity(dispatch_path),
                                 elapsed_ms=watch["elapsed_ms"], reason=watch["error"],
                                 selected_gpu=selected, actual_adapter=actual,
                                 actual_adapter_verified=bool(actual),
                                 last_watch_phase=watch["events"][-1]["lifecycle_phase"],
                                 observed_null_requests=len(request_events),
                                 no_complete_lifecycle_inference=True, capture_unchanged=True))
            continue
        require(watch["completed"] and watch["exit_code"] == 0 and watch["constructor_hits"] == 1 and
                watch["destructor_hits"] == 1 and watch["request_hits"] == 2, "Incomplete watch lifecycle")
        writes = [e for e in watch["events"] if e["hits"] & 4]
        require(len(writes) == watch["field_writes"], "Incorrect field write count")
        summary = dict(name=name, watch=identity(path), elapsed_ms=watch["elapsed_ms"],
                       field_writes=len(writes), live_nonzero=watch["nonzero_live_field_writes"])
        if calibration:
            require(len(writes) == 36 and len({e["thread_id"] for e in writes}) == 3 and
                    watch["nonzero_live_field_writes"] > 0, "Calibration failed")
            calibrations.append(summary)
            continue
        report_path = path.with_name("dispatch.json")
        dispatch = read(report_path)
        validate_dispatch(dispatch)
        require(digest(path) == row["watch"]["sha256"] and digest(report_path) == row["dispatch"]["sha256"],
                "Runtime artifact changed")
        require(len(writes) == 2 and watch["nonzero_live_field_writes"] == 0 and
                writes[0]["lifecycle_phase"] == "constructing" and writes[0]["written_engine"] == "0x0" and
                writes[1]["lifecycle_phase"] == "destroying", "Engine lifecycle findings changed")
        require(all(e["request_engine"] == "0x0" for e in watch["events"] if e["hits"] & 8),
                "Non-null engine request")
        key = (dispatch["event"], dispatch["requested_vendor"], "no-first-playbacks" in dispatch["settings"])
        require(key not in seen, "Duplicate lifecycle")
        seen.add(key)
        summary.update(dispatch=identity(report_path), adapter=dispatch["replay_adapter"],
                       capture_sha256=dispatch["capture_sha256"], event=dispatch["event"],
                       settings=dispatch["settings"], result_callbacks=0)
        runs.append(summary)
        for name in ("vendor", "device", "luid", "after", "callbacks"):
            changed = copy.deepcopy(dispatch)
            if name in ("vendor", "device", "luid"):
                changed["replay_adapter"][name] ^= 1
            elif name == "after":
                changed["replay_adapter_after"]["luid"] ^= 1
            else:
                changed["requests"][0]["result_callbacks"].append({})
            try:
                validate_dispatch(changed)
            except ValueError as error:
                controls[str(key) + name] = str(error)
            else:
                raise ValueError("Altered evidence accepted")
    require(len(calibrations) == 3 and seen == set(itertools.product((113, 876, 200),
            (4318, 32902), (False, True))), "Incomplete explicit-adapter matrix")
    exports = []
    static = args.root / "static-verified"
    binary = read(static / "binary.json")
    require(binary["sha256"] == PLAYER_SHA256 and digest(Path(binary["path"])) == PLAYER_SHA256,
            "Pinned installed player changed")
    for item in read(static / "evidence-sha256.json"):
        path = static / item["name"]
        require(digest(path) == item["sha256"], "Static artifact changed")
        if path.suffix == ".txt":
            text = path.read_text()
            require(text.splitlines()[-1].startswith("COMPLETE ") and
                    PLAYER_SHA256 in text.splitlines()[0] and "\nNO FUNCTION " not in text,
                    "Incomplete Ghidra export")
        exports.append(identity(path))
    selector_path = args.root / "selector-probe/selector.json"
    selector = read(selector_path)
    require(selector["player_unchanged"] and selector["player_sha256"] == PLAYER_SHA256 and
            selector["source_sha256"] == digest(Path(__file__).with_name("probe_original_gpu_selector.py")) and
            len(selector["cases"]) == 16 and all(r["actual"] == r["expected"] and r["record_unchanged"]
            for r in selector["cases"]), "Selector predicate probe failed")
    ledger = dict(schema=1, scope="Explicit GPU selection and bounded original debugger lifecycles",
                  engine_recovered=False, shader_debug_parity=False, complete_release_acceptance=False,
                  installed_player_unchanged=True,
                  manifests=[identity(p) for p in manifests], calibrations=calibrations, runs=runs,
                  failed_runs=failures, all_lifecycles_completed=not failures,
                  static_exports=exports, selector_probe=identity(selector_path),
                  summarizer=identity(Path(__file__)),
                  evidence_corruptions_rejected=controls)
    with args.out.open("x", encoding="utf-8") as out:
        json.dump(ledger, out, indent=2)
        out.write("\n")
    print(f"Validated 3 calibrations, {len(runs)} completed lifecycles, {len(failures)} retained failures, "
          f"16 predicate cases and {len(controls)} corruptions")


if __name__ == "__main__":
    main()
