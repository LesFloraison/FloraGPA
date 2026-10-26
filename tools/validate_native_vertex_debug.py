"""Check a native VS job against a separately observed selected invocation.

The reference is produced by FloraDxbcVertexInputProbe --compare-native.
This validates selected register outputs, provenance and session transport;
it does not certify arbitrary shaders or complete debugger parity.
"""
import argparse
import json
from pathlib import Path
import subprocess

from validate_dxbc_execution_trace import digest, register, require, value
from validate_native_debug_session import validate as validate_archive


def validate(directory, reference_path, session_probe, output):
    directory, output = Path(directory), Path(output)
    read = lambda path: json.loads(Path(path).read_text(encoding="utf-8"))
    reference = read(reference_path)
    job, result = read(directory / "job.json"), read(directory / "result.json")
    require(result["ok"] is True and result["request_id"] == job["request_id"], "Stale or incomplete job")
    require(job["action"] == result["context"]["action"] == "debug-vertex", "Wrong analysis action")
    data, invocation = result["data"], result["data"]["invocation"]
    require(data["format"] == "flora.native-vertex-debug" and data["version"] in (1, 2), "Wrong VS result format")
    require(data["effective_frame_sha256"] == reference["frame_sha256"] and
            data["shader_sha256"] == reference["shader_sha256"], "Reference shader or frame differs")
    require(reference["status"] == "ok" and reference["execution_complete"] is True and
            reference["native_comparison"]["exact"] is True, "Reference did not match native shader execution")
    override = job["options"].get("index") if data["version"] == 2 else None
    expected_status = "explicit_vertex_index_override" if override is not None else "eligible_in_observed_replay"
    require(invocation["status"] == expected_status and
            invocation["predicate"]["allows_execution"] is True, "Missing invocation eligibility")
    if data["version"] == 2:
        require(invocation["index_override"] == override and
                invocation["vertex_index_source"] == ("explicit_override" if override is not None else "draw"),
                "Wrong VS index override provenance")
        require(invocation["observed_vertex_index"] == reference["invocation"].get(
                    "observed_vertex_index", reference["invocation"]["vertex_index"]),
                "Observed vertex index differs from reference")
    for key in ("gpa_event", "gpa_shader", "input_reference", "instance", "vertex_index", "vertex_id",
                "draw_parameters", "index_buffer", "index_offset", "index_format", "layout", "topology"):
        require(invocation[key] == reference["invocation"][key], "Invocation differs: " + key)
    for module in result["execution"]["loaded_modules"]:
        path = module.replace("\\", "/").lower()
        require("renderdoc" not in path and "/intelswtools/gpa/" not in path and
                path.rsplit("/", 1)[-1] not in {"dx11_player.dll", "dx11_playback.dll", "shimloader64.dll", "shimd3d64.dll"},
                "External analysis module loaded")
    registers, history = {}, []
    with (directory / "trace.jsonl").open(encoding="utf-8") as stream:
        header = json.loads(next(stream))
        require(header["stage"] == "vs" and header["scope"] == "one_vertex" and header["selected_lane"] == 0,
                "Wrong trace invocation scope")
        require(not {"group", "thread", "group_size"}.intersection(header), "VS trace has compute coordinates")
        require(header.get("index_override") == override, "VS journal index override differs")
        require(header["adapter"] == {"luid": reference["adapter_luid"], "vendor": reference["adapter_vendor"],
                                      "device": reference["adapter_device"]}, "Reference adapter differs")
        for raw in stream:
            row = json.loads(raw)
            if row["record"] == "register":
                require(row["lane"] == 0, "VS has multiple lanes")
                registers[register(row["register"])] = value(row["value"])
            elif row["record"] == "step":
                require(row["lane"] == 0 and row["sequence"] == row["lane_sequence"] == len(history),
                        "Discontinuous VS sequence")
                require(not any(row[key] for key in ("buffer_changes", "shared_changes", "texture_changes",
                                                     "counter_changes")), "Reference gate expects read-only shaders")
                for change in row["register_changes"]:
                    key = register(change["register"])
                    require(registers.get(key, ((0, 0, 0, 0), 0)) == value(change["before"]), "Broken register chain")
                    registers[key] = value(change["after"])
                history.append(row)
    require(len(history) == data["trace"]["steps"] == reference["execution_steps"], "Step count differs")
    expected = {(item["kind"], tuple(item["indices"])): (tuple(item["bits"]), item["valid"])
                for item in reference["final_registers"]}
    require(registers == expected, "Final raw registers or validity differ from reference execution")
    for component in reference["native_comparison"]["components"]:
        bits, valid = registers[(2, (component["register"],))]
        c = component["component"]
        require(valid & (1 << c) and bits[c] == component["native_bits"], "Native output component differs")
    for row in reversed(history):
        for change in reversed(row["register_changes"]):
            key = register(change["register"])
            require(registers[key] == value(change["after"]), "Broken reverse register chain")
            registers[key] = value(change["before"])
    for item in reference["input_registers"]:
        require(registers[(item["kind"], tuple(item["indices"]))] == (tuple(item["bits"]), item["valid"]),
                "Input register restoration differs")
    session_report, archive = output.with_suffix(".session.json"), output.with_suffix(".zip")
    subprocess.run([str(Path(session_probe).resolve()), str(directory.resolve()), str(session_report.resolve()),
                    str(archive.resolve())], check=True, capture_output=True, timeout=180)
    archive_report = validate_archive(archive, directory, session_probe)
    summary = {"schema": 1, "passed": True, "complete_release_acceptance": False,
               "reference_sha256": digest(Path(reference_path).read_bytes()),
               "context_id": result["context_id"], "steps": len(history),
               "output_components": len(reference["native_comparison"]["components"]),
               "raw_registers_match": True, "input_register_round_trip": True,
               "session": read(session_report), "archive": archive_report}
    output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("reference", type=Path)
    parser.add_argument("--session-probe", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    report = validate(args.directory, args.reference, args.session_probe, args.out)
    print(json.dumps({"passed": report["passed"], "steps": report["steps"]}))
