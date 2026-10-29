"""Validate native CLI compute artifacts against an independent input probe.

The reference probe must describe the same unedited frame, shader, adapter and
selected group. Its initial/final storage digests remain independent of the
journal writer. This is a captured diagnostic gate, not release acceptance.
"""
import argparse
import copy
import json
from pathlib import Path
import tempfile

from validate_dxbc_execution_trace import digest, require, validate


def validate_job(directory, reference, expected_groups, worker, reader_report=None):
    directory = Path(directory)
    result = json.loads((directory / "result.json").read_text(encoding="utf-8"))
    job = json.loads((directory / "job.json").read_text(encoding="utf-8"))
    require(result["ok"] is True and result["schema"] == 1 and result["backend"] == "dx11-native/6", "Invalid native result")
    require(result["request_id"] == job["request_id"], "Request identity mismatch")
    context = result["context"]
    for key in ("schema", "backend", "backend_sha256", "frame_sha256", "experiment_sha256", "action", "options"):
        require(context[key] == job[key], "Stale native context: " + key)
    canonical = json.dumps(context, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    require(result["context_id"] == digest(canonical), "Context digest mismatch")
    adapter = context["adapter"]
    luid = ((adapter["luid_high"] & 0xffffffff) << 32) | adapter["luid_low"]
    require((adapter["vendor"], adapter["device"], luid) ==
            (reference["adapter_vendor"], reference["adapter_device"], reference["adapter_luid"]), "Actual adapter mismatch")
    require(context["action"] == "debug-thread" and context["experiment_sha256"] is None, "Reference gate requires an unedited compute job")
    require(context["backend_sha256"] == digest(Path(worker).read_bytes()), "Worker binary mismatch")
    data = result["data"]
    require(data["format"] == "flora.native-compute-debug" and data["version"] == 1, "Unsupported compute result")
    require(data["captured_gpu_schedule"] is False, "Invalid original schedule claim")
    require(data["trace_file"] == "trace.jsonl", "Unexpected native trace path")
    require(data["shader_file"] == "shader.dxbc", "Unexpected native shader path")
    require(set(result["artifacts"]) == {"trace.jsonl", "debug.json", "shader.dxbc"}, "Unexpected native artifacts")
    for name, metadata in result["artifacts"].items():
        raw = (directory / name).read_bytes()
        require(len(raw) == metadata["bytes"] and digest(raw) == metadata["sha256"], "Artifact digest mismatch")
    require(json.loads((directory / "debug.json").read_text(encoding="utf-8")) == data, "Debug metadata mismatch")
    require(context["frame_sha256"] == reference["frame_sha256"] == data["effective_frame_sha256"], "Frame provenance mismatch")
    require(data["gpa_shader"] == reference["shader"], "Shader identity mismatch")
    require(data["shader_sha256"] == reference["shader_sha256"] == digest((directory / "shader.dxbc").read_bytes()),
            "Effective shader artifact identity mismatch")
    invocation = data["invocation"]
    require(invocation["gpa_event"] == reference["event"] == context["options"]["gpa_event"], "Event identity mismatch")
    require(invocation["status"] == "eligible_in_observed_replay", "Missing eligibility evidence")
    require(invocation["dispatch_kind"] == "direct" and invocation["dispatch_groups"] == expected_groups, "Unexpected effective dispatch")
    require(invocation["predicate"]["source"] == "unbound" and invocation["predicate"]["resource"] == 0, "Reference gate expects unpredicated execution")
    require(invocation["predicate"]["original_query_recovered"] is False, "Invalid captured query claim")
    require(invocation["group"] == [0, 0, 0] and invocation["thread"] == [0, 0, 0], "Reference group selection differs")
    for module in result["execution"]["loaded_modules"]:
        normalized = module.replace("\\", "/").lower()
        name = normalized.rsplit("/", 1)[-1]
        require("renderdoc" not in name and "/intelswtools/gpa/" not in normalized and
                name not in {"dx11_player.dll", "dx11_playback.dll", "shimloader64.dll", "shimd3d64.dll"},
                "External analysis module loaded")
    expected = copy.deepcopy(reference)
    expected["trace"].update(data["trace"])
    validation = validate(directory / "trace.jsonl", expected, reader_report)
    if reader_report is not None:
        model = reader_report["debug_model"]
        require(model["shader_sha256"] == data["shader_sha256"] and
                model["selected_steps"] == data["trace"]["selected_steps"] and
                model["terminal"] == {"state_boundary": "terminal_group", "global_step": data["trace"]["steps"],
                                      "instruction": None} and model["register_round_trip"] is True and
                model["configuration_round_trip"] is True, "Native debug model identity or restoration mismatch")
    return {"schema": 1, "passed": True, "complete_release_acceptance": False,
            "scope": "native_cli_identity_and_captured_group_trace_continuity",
            "result_sha256": digest((directory / "result.json").read_bytes()),
            "context_id": result["context_id"], "invocation": invocation,
            "execution": result["execution"], "trace_validation": validation}


def negative_checks(directory, reference, expected_groups, worker):
    directory = Path(directory)
    names = ("backend", "request", "context_digest", "adapter", "worker_digest", "trace_path",
             "query_claim", "shader", "eligibility", "external_module", "artifact_bytes",
             "shader_path", "shader_digest", "shader_bytes")
    results = []
    with tempfile.TemporaryDirectory(prefix="flora-compute-negative-") as temporary:
        output = Path(temporary)
        for name in names:
            for filename in ("result.json", "debug.json", "job.json", "trace.jsonl", "shader.dxbc"):
                (output / filename).write_bytes((directory / filename).read_bytes())
            result = json.loads((output / "result.json").read_text(encoding="utf-8"))
            job = json.loads((output / "job.json").read_text(encoding="utf-8"))
            if name == "backend": result["backend"] = "dx11-native/2"
            elif name == "request": result["request_id"] = "different"
            elif name == "context_digest": result["context_id"] = "0" * 64
            elif name == "adapter": result["context"]["adapter"]["vendor"] ^= 1
            elif name == "worker_digest":
                result["context"]["backend_sha256"] = job["backend_sha256"] = "0" * 64
            elif name == "trace_path": result["data"]["trace_file"] = "../trace.jsonl"
            elif name == "query_claim": result["data"]["invocation"]["predicate"]["original_query_recovered"] = True
            elif name == "shader": result["data"]["gpa_shader"] += 1
            elif name == "shader_path": result["data"]["shader_file"] = "../shader.dxbc"
            elif name == "shader_digest": result["data"]["shader_sha256"] = "0" * 64
            elif name == "shader_bytes":
                raw = (output / "shader.dxbc").read_bytes() + b"changed"
                (output / "shader.dxbc").write_bytes(raw)
                result["artifacts"]["shader.dxbc"] = {"bytes": len(raw), "sha256": digest(raw)}
            elif name == "eligibility": result["data"]["invocation"]["status"] = "unknown"
            elif name == "external_module": result["execution"]["loaded_modules"].append("C:/tools/renderdoc.dll")
            elif name == "artifact_bytes":
                with (output / "trace.jsonl").open("ab") as file: file.write(b" ")
            if name in ("adapter", "worker_digest"):
                canonical = json.dumps(result["context"], ensure_ascii=False, sort_keys=True, separators=(",", ":"))
                result["context_id"] = digest(canonical.encode("utf-8"))
            if name in ("trace_path", "query_claim", "shader", "eligibility", "shader_path", "shader_digest"):
                raw = json.dumps(result["data"]).encode("utf-8")
                (output / "debug.json").write_bytes(raw)
                result["artifacts"]["debug.json"] = {"bytes": len(raw), "sha256": digest(raw)}
            (output / "result.json").write_text(json.dumps(result), encoding="utf-8")
            (output / "job.json").write_text(json.dumps(job), encoding="utf-8")
            try:
                validate_job(output, reference, expected_groups, worker)
            except (ValueError, KeyError, IndexError) as error:
                results.append({"case": name, "rejected": True, "reason": str(error)})
            else:
                raise ValueError("Invalid native compute result accepted: " + name)
    return {"passed": True, "cases": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--dispatch-groups", type=int, nargs=3, required=True)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--negative-out", type=Path)
    parser.add_argument("--reader-report", type=Path)
    args = parser.parse_args()
    reference = json.loads(args.reference.read_text(encoding="utf-8"))
    reader_report = json.loads(args.reader_report.read_text(encoding="utf-8")) if args.reader_report else None
    result = validate_job(args.directory, reference, args.dispatch_groups, args.worker, reader_report)
    if args.reader_report:
        result["reader_model_sha256"] = digest(args.reader_report.read_bytes())
        result["reader_model_states_match"] = True
    result["reference_sha256"] = digest(args.reference.read_bytes())
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if args.negative_out:
        negative = negative_checks(args.directory, reference, args.dispatch_groups, args.worker)
        args.negative_out.write_text(json.dumps(negative, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "steps": result["trace_validation"]["steps"]}))


if __name__ == "__main__":
    main()
