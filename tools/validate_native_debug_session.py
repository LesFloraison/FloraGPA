"""Independently verify exported native debug archives and optional C++ reload.

This validates transport and session continuity, not shader execution correctness
or full debugger parity. Original captures and archive payloads stay outside Git.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def validate(archive, original=None, probe=None):
    names = {"job.json", "result.json", "debug.json", "shader.dxbc", "trace.jsonl", "state.json"}
    with zipfile.ZipFile(archive) as zipped:
        require(len(zipped.namelist()) == len(names) and set(zipped.namelist()) == names,
                "Unexpected or duplicate archive entries")
        require(sum(entry.file_size for entry in zipped.infolist()) <= 300 * 1024 * 1024,
                "Archive allocation budget exceeded")
        require(zipped.testzip() is None, "Archive CRC mismatch")
        payloads = {name: zipped.read(name) for name in names}
    result = json.loads(payloads["result.json"])
    job = json.loads(payloads["job.json"])
    state = json.loads(payloads["state.json"])
    require(result["ok"] is True and result["request_id"] == job["request_id"], "Incomplete or stale result")
    for key in ("schema", "backend", "backend_sha256", "frame_sha256", "experiment_sha256", "action", "options"):
        require(result["context"][key] == job[key], "Context differs from job: " + key)
    context_bytes = json.dumps(result["context"], sort_keys=True, ensure_ascii=False,
                               separators=(",", ":")).encode("utf-8")
    require(result["context_id"] == digest(context_bytes), "Context digest mismatch")
    require(set(result["artifacts"]) == {"debug.json", "shader.dxbc", "trace.jsonl"}, "Unexpected manifest")
    for name, entry in result["artifacts"].items():
        require(len(payloads[name]) == entry["bytes"] and digest(payloads[name]) == entry["sha256"],
                "Artifact differs from manifest: " + name)
        if original is not None:
            require(payloads[name] == (Path(original) / name).read_bytes(), "Original artifact changed: " + name)
    if original is not None:
        for name, value in (("job.json", job), ("result.json", result)):
            require(value == json.loads((Path(original) / name).read_text(encoding="utf-8")),
                    "Original metadata changed: " + name)
    require(json.loads(payloads["debug.json"]) == result["data"], "Debug metadata differs from result")
    require(state["format"] == "flora.native-execution-debug-report" and state["version"] == 1,
            "Unsupported debug state")
    require(state["model"]["shader_sha256"] == result["data"]["shader_sha256"] == digest(payloads["shader.dxbc"]),
            "State shader identity mismatch")
    require(state["model"]["trace_sha256"] == digest(payloads["trace.jsonl"]), "State trace identity mismatch")
    reload_report = None
    if probe is not None:
        with tempfile.TemporaryDirectory(prefix="flora-debug-archive-check-") as temporary:
            directory = Path(temporary)
            for name, data in payloads.items():
                (directory / name).write_bytes(data)
            output = directory / "reload.json"
            subprocess.run([str(Path(probe).resolve()), str(directory), str(output), str(directory / "reexport.zip")],
                           check=True, capture_output=True, text=True, timeout=180)
            reload_report = json.loads(output.read_text(encoding="utf-8"))
            require(reload_report["passed"] is True and reload_report["resumed"] is True,
                    "C++ session reload did not restore the saved state")
            require(reload_report["context_id"] == result["context_id"], "Reload changed context")
            with zipfile.ZipFile(directory / "reexport.zip") as repeated:
                require(json.loads(repeated.read("state.json")) ==
                        {key: value for key, value in state.items() if key not in ("analysis_context", "analysis_context_id")},
                        "Re-export changed the debug state or configuration")
    return {"schema": 1, "passed": True, "complete_release_acceptance": False,
            "archive_sha256": digest(Path(archive).read_bytes()), "context_id": result["context_id"],
            "entries": sorted(names), "original_artifacts_compared": original is not None,
            "position": state["position"], "watches": state["watches"], "reload": reload_report}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--original", type=Path)
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    report = validate(args.archive, args.original, args.probe)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))
