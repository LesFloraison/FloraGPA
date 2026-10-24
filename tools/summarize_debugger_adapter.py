"""Validate the retained adapter/wire research session without claiming engine recovery."""

import argparse
import hashlib
import json
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    def identity(path):
        return dict(path=path.resolve().relative_to(repo).as_posix(), sha256=digest(path))
    static = args.root / "verified"
    wire_path = args.root / "wire-cross-module/wire.json"
    process_path = wire_path.with_name("process.json")
    wire, process = read(wire_path), read(process_path)
    require(process["exit_code"] == 0 and not process["timed_out"], "Original probe failed")
    require(wire["completed"] and wire["binary_unchanged"] and wire["player_unchanged"] and
            not wire["engine_recovered"] and not wire["shader_debug_parity"], "Invalid probe scope/result")
    require(len(wire["cases"]) == 9 and all(case["matched"] for case in wire["cases"]),
            "Incomplete original codec cases")
    require(wire["source_sha256"] == digest(repo / "tools/probe_original_debugger_wire.py"),
            "Wire probe source changed")
    ledger = dict(schema="FloraGPA debugger adapter evidence 1", date="2026-10-24",
                  engine_recovered=False, shader_debug_parity=False,
                  scope="Pinned DX11 legacy adapter and synthetic codec behavior only; no original shader execution",
                  binaries=read(static / "binaries.json"), sources=[], static_exports=[],
                  process=identity(process_path), wire=identity(wire_path), cases=wire["cases"],
                  dispatch={k: v for k, v in wire.items() if k.endswith("_rva")})
    for name in ["tools/probe_original_debugger_wire.py", "tools/summarize_debugger_adapter.py",
                 "tools/ghidra/ExportFieldStores.java", "tools/ghidra/ExportFunctionEvidence.java",
                 "tools/ghidra/ExportSymbolEvidence.java", "tools/ghidra/Invoke-DebuggerAdapterResearch.ps1"]:
        ledger["sources"].append(identity(repo / name))
    for entry in read(static / "evidence-sha256.json"):
        path = static / entry["name"]
        require(digest(path) == entry["sha256"], f"Changed static evidence: {path}")
        if path.suffix == ".txt":
            lines = path.read_text(encoding="utf-8-sig").splitlines()
            require(lines[-1].startswith("COMPLETE ") and not any(line.startswith("NO FUNCTION ") for line in lines),
                    f"Incomplete static evidence: {path}")
            ledger["static_exports"].append(dict(**identity(path), completion=lines[-1]))
    require(len(ledger["static_exports"]) == 17, "Missing adapter exports")
    failed = args.root / "framedb-adapter-stores.log"
    require("NullPointerException" in failed.read_text(encoding="utf-8-sig"), "Missing retained scanner failure")
    ledger["retained_failure"] = dict(**identity(failed),
        reason="Unresolved memory operands can have null Ghidra reference types; replaced with explicit candidate opcode matching")
    negative_path = args.root / "integrity-negative.json"
    if negative_path.exists():
        negative = read(negative_path)
        require(negative["rejected"] and negative["exit_code"] != 0 and
                "Changed static evidence:" in negative["error"], "Invalid integrity rejection record")
        ledger["integrity_negative"] = identity(negative_path)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(ledger, indent=2) + "\n", encoding="utf-8")
    print("Validated 17 exports and 9 original codec cases; concrete engine remains unresolved")


if __name__ == "__main__":
    main()
