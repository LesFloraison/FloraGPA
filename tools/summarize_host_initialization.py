"""Validate the bounded host-initialization study, retaining its unresolved engine gate."""
import argparse
import copy
import json
from pathlib import Path
import struct

from probe_original_debugger_host import PLAYER_SHA256, digest
from summarize_debugger_gpu import require, validate_dispatch

CAPTURE_SHA256 = "fc7b7f7de20a578da2977b2ff91d96f5cf80887b393bc85859be8cab1af91c8e"


def validate_graphs(report):
    validate_dispatch(report)
    require(report["capture_sha256"] == CAPTURE_SHA256 and report["event"] == 113,
            "Unexpected capture selection")
    require(not report["engine_recovered"] and not report["shader_debug_parity"],
            "Unverified engine claim")
    snapshots = report["snapshots"]
    require([s["label"] for s in snapshots] == ["loaded", "opened", "replayed", "after_control_request",
            "after_observed_request", "replayed_after_requests", "closed"], "Missing lifecycle boundary")
    graphs = [s["host_graph"] for s in snapshots[1:-1]]
    require(all(g == graphs[0] for g in graphs), "Host graph changed during lifecycle")
    for snapshot in snapshots[1:-1]:
        graph = snapshot["host_graph"]
        host = graph["host"]
        require(int(host, 16) != 0 and host == snapshot["host"] and
                graph["engine"] == snapshot["engine"] == "0x0", "Unexpected host or engine")
        expected = {"subobject_08_owner": "0x18", "subobject_48_owner": "0x48",
                    "request_manager_owner": "0x190"}
        require(set(graph["aliases"]) == set(expected), "Missing owner alias")
        for name, offset in expected.items():
            row = graph["aliases"][name]
            require(row == dict(offset=offset, pointer=host, matches_host=True), "Owner alias mismatch")
        api, query = graph["api"], graph["query"]
        require(api["offset"] == "0x1b8" and int(api["pointer"], 16) != 0 and
                api["vtable_rva"] == "0x17ae40" and api["owner"] == host and api["owner_matches_host"] and
                api["slots"] == ["0x3a390", "0x452e0", "0x4ec90", "0x3d5f0"], "API manager mismatch")
        # These four observations selected D3D11QueryManager. A GM result is new evidence,
        # not automatically a success under this ledger's narrower expectation.
        require(query["offset"] == "0x1c8" and int(query["pointer"], 16) != 0 and
                query["vtable_rva"] == "0x17d668" and query["api"] == api["pointer"] and
                query["api_matches_host_manager"] and
                query["slots"] == ["0x50ab0", "0x6f470", "0x52940", "0x6f5d0"], "Query manager mismatch")
        require(any(Path(p).name.lower() == "dx11_player.dll" for p in snapshot["modules"]),
                "Missing loaded player")
    return graphs[0]


def validate_synthetic(report):
    require(report["completed"] and not report["engine_recovered"] and report["player_sha256"] == PLAYER_SHA256,
            "Incomplete synthetic report")
    expected_cases = {(rva, flag) for rva in ("0xa0130", "0x9bab0", "0x8e510") for flag in (False, True)}
    require(len(report["rows"]) == len(expected_cases), "Incomplete synthetic matrix")
    seen = set()
    for row in report["rows"]:
        key = (row["rva"], row["no_first_playbacks"])
        require(key in expected_cases and key not in seen, "Unexpected synthetic case")
        seen.add(key)
        subobject, destination = {"0xa0130": (8, 0x18), "0x9bab0": (0x48, 0x48),
                                 "0x8e510": (0xf8, 0x190)}[row["rva"]]
        require(row["subobject"] == hex(subobject) and row["destination"] == hex(destination),
                "Synthetic layout changed")
        require(row["before"] == "a5" * 0x440 and row["unchanged_engine"], "Synthetic guard changed")
        expected = bytearray.fromhex(row["before"])
        require(int(row["host"], 16) != 0, "Null synthetic host")
        struct.pack_into("<Q", expected, 0x20 + destination, int(row["host"], 16))
        if row["rva"] == "0x8e510":
            expected[0x20 + 0x199] = int(not row["no_first_playbacks"])
            expected[0x20 + 0x19a] = 0
        require(row["after"] == expected.hex(), "Unexpected synthetic memory write")


def rejected(validate, value):
    try:
        validate(value)
    except (ValueError, KeyError):
        return 1
    raise ValueError("Corrupted evidence was accepted")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    read = lambda p: json.loads(p.read_text(encoding="utf-8-sig"))
    identity = lambda p: dict(path=p.resolve().relative_to(repo).as_posix(), bytes=p.stat().st_size, sha256=digest(p))
    manifest = read(args.root / "final/runs.json")
    require(manifest["completed"] and manifest["capture_sha256"] == CAPTURE_SHA256, "Incomplete runner")
    require([r["name"] for r in manifest["runs"]] == ["synthetic", "gf2-nvidia-controlled", "gf2-nvidia-default",
            "gf2-intel-controlled", "gf2-intel-default"], "Incomplete run matrix")
    for source in manifest["sources"]:
        require(digest(Path(source["path"])) == source["sha256"], "Runtime source changed")
    controls, summaries = 0, []
    for run in manifest["runs"]:
        require(run["exit_code"] == 0 and not run["timed_out"], "Probe failed")
        if run["name"] == "synthetic":
            report = read(args.root / "final/synthetic/synthetic.json")
            validate_synthetic(report)
            require(report["source_sha256"] == digest(repo / "tools/probe_original_host_initialization.py"),
                    "Synthetic source changed")
            for index in range(6):
                changed = copy.deepcopy(report)
                data = bytearray.fromhex(changed["rows"][index]["after"])
                data[0x20 + 0x3c0] ^= 1
                changed["rows"][index]["after"] = data.hex()
                controls += rejected(validate_synthetic, changed)
            continue
        report = read(args.root / "final" / run["name"] / "dispatch.json")
        graph = validate_graphs(report)
        require(report["source_sha256"] == digest(repo / "tools/probe_original_debugger_host.py"),
                "Runtime source changed")
        require(digest(Path(report["capture"])) == CAPTURE_SHA256, "Capture changed")
        summaries.append(dict(name=run["name"], elapsed_ms=run["elapsed_ms"],
            adapter=report["replay_adapter"], api_table=graph["api"]["vtable_rva"],
            query_table=graph["query"]["vtable_rva"], graph_boundaries=5, engine="null"))
        for key in ("alias", "api_owner", "query_api", "query_slot", "engine", "boundary"):
            changed = copy.deepcopy(report)
            for snapshot in changed["snapshots"][1:-1]:
                target = snapshot["host_graph"]
                if key == "alias": target["aliases"]["subobject_08_owner"]["pointer"] = "0x0"
                if key == "api_owner": target["api"]["owner"] = "0x0"
                if key == "query_api": target["query"]["api"] = "0x0"
                if key == "query_slot": target["query"]["slots"][1] = "0x902b0"
                if key == "engine": target["engine"] = "0x1"
            if key == "boundary": changed["snapshots"].pop(2)
            controls += rejected(validate_graphs, changed)
    static = args.root / "verified"
    expected = {"host": 8, "managers": 7, "tables": 3, "debug-symbols": 22, "query-symbols": 50}
    for name, count in expected.items():
        text = (static / (name + ".txt")).read_text(encoding="utf-8")
        require(text.splitlines()[0] == f"PROGRAM dx11_player.dll SHA256 {PLAYER_SHA256} BASE 180000000",
                "Wrong static binary")
        require(text.splitlines()[-1] == f"COMPLETE {count}" and "NO FUNCTION " not in text,
                "Incomplete static export")
        require("ERROR REPORT SCRIPT ERROR" not in (static / (name + ".log")).read_text(encoding="utf-8"),
                "Ghidra script failed")
    for row in read(static / "evidence-sha256.json"):
        require(digest(static / row["name"]) == row["sha256"], "Static export changed")
    binary = read(static / "binary.json")
    require(digest(Path(binary["path"])) == binary["sha256"] == PLAYER_SHA256, "Player changed")
    sources = [repo / p for p in ("tools/probe_original_host_initialization.py", "tools/probe_original_debugger_host.py",
        "tools/summarize_host_initialization.py", "tools/summarize_debugger_gpu.py",
        "tools/ghidra/Invoke-HostInitializationResearch.ps1", "tools/ghidra/ExportFunctionEvidence.java",
        "tools/ghidra/ExportAddressEvidence.java", "tools/ghidra/ExportSymbolEvidence.java")]
    ledger = dict(schema="FloraGPA host initialization research 1", player=binary, synthetic_cases=6,
        observations=summaries, corruption_controls=controls, engine_recovered=False, p1_closed=False,
        preliminary="Root exports and gf2-nvidia-controlled are exploratory only; the preliminary query owner offset was incorrect.",
        sources=[identity(p) for p in sources], raw=[identity(p) for p in sorted(args.root.rglob("*")) if p.is_file()])
    args.out.write_text(json.dumps(ledger, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(dict(synthetic_cases=6, lifecycles=len(summaries), corruption_controls=controls,
                          static_exports=len(expected), engine_recovered=False)))


if __name__ == "__main__":
    main()
