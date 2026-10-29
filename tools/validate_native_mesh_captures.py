"""Run serial GPA mesh jobs and compare VS input assembly with final SO bytes."""
import argparse
import copy
import csv
import hashlib
import io
import json
import math
import os
from pathlib import Path
import struct
import subprocess


CAPTURES = {
    "gf2": ("GF2_Exilium_2026_03_03__00_19_35.gpa_frame",
            "fc7b7f7de20a578da2977b2ff91d96f5cf80887b393bc85859be8cab1af91c8e"),
    "bf1": ("bf1_2026_01_21__16_53_05.gpa_frame",
            "26044d33cb6c3087c2204b7d57664203741a679f988430b7d05b24af582e0620"),
    "helldivers": ("helldivers2_2026_04_02__18_02_58.gpa_frame",
                  "97915a80c7617e9a67068a5d85b87f1b2a7883faa9831428084ebd7270f37139"),
}
CASES = [("gf2", 113, 0), ("gf2", 181, 0), ("bf1", 876, 0),
         ("bf1", 1415, 0), ("bf1", 1415, 8), ("bf1", 2010, 0), ("helldivers", 200, 0)]

ADDITIONAL_CASES = [("bf1", 1415, 1), ("bf1", 1415, 4), ("helldivers", 200, 11)]
CASE_SETS = ("baseline", "additional-instances", "all")


def selected_cases(name):
    if name not in CASE_SETS:
        raise ValueError("Unknown case set")
    if name == "baseline":
        return CASES
    if name == "additional-instances":
        return ADDITIONAL_CASES
    return CASES + ADDITIONAL_CASES


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def same_float(left, right):
    return ((math.isnan(left) and math.isnan(right)) or
            (left == right and (left != 0 or math.copysign(1, left) == math.copysign(1, right))))


def validate_modules(modules):
    require(bool(modules), "module evidence")
    for module in modules:
        name = module.lower().replace("\\", "/")
        require("renderdoc" not in name and "intelswtools/gpa" not in name, "external analyzer loaded")
        require(name.rsplit("/", 1)[-1] not in
                {"dx11_player.dll", "dx11_playback.dll", "shimd3d64.dll", "gpa.dll"},
                "private GPA module loaded outside its installation directory")


def compact_sources(data, raw, identities, count=None):
    stride = data["stride"]
    require(len(identities) == (data["vertices"] if count is None else count), "identity count")
    unique, variants, keys = {}, {}, []
    for i, identity in enumerate(identities):
        owner = tuple(identity[:2])
        key = owner + (raw[i * stride:(i + 1) * stride],)
        if key not in unique:
            variant = variants.get(owner, 0)
            variants[owner] = variant + 1
            unique[key] = (owner + (variant,), i)
        keys.append(key)
    order = sorted(unique, key=lambda key: unique[key][0])
    remap = {key: i for i, key in enumerate(order)}
    return [unique[key][1] for key in order], [remap[key] for key in keys]


def presentation_storage(folder, data, raw):
    require(data["stride"] > 0 and len(raw) == data["vertices"] * data["stride"], "selected storage size")
    identities = load(folder / "identities.json") if "vertex_identity" in data else []
    if "vertex_storage" not in data:
        return raw, identities
    storage, selection = data["vertex_storage"], data["instance_selection"]
    stride, selected = data["stride"], data["vertices"]
    extra = (folder / "remaining_vertices.bin").read_bytes()
    extra_ids = load(folder / "remaining_identities.json")
    total = storage["vertices"]
    require(storage["schema"] == 1 and storage["scope"] == "remaining" and storage["stride"] == stride and
            storage["selected_vertices"] == selected and total >= selected and
            len(raw) == selected * stride and len(extra) == (total - selected) * stride, "remaining storage size")
    require(storage["full_vertices"] == selection["full_capture"]["vertices"] and
            storage["full_sha256"] == selection["full_capture"]["sha256"] and
            storage["first_vertex"] >= 0 and storage["first_vertex"] + total == storage["full_vertices"],
            "remaining capture provenance")
    if storage["first_vertex"] == 0:
        require(storage["sha256"] == storage["full_sha256"], "full remaining capture digest")
    if "first_vertex" not in selection:
        require(data["requested_stage"] == "vs-inputs", "missing native instance range")
    expected_first = selection.get("first_vertex", selected * selection["instance"])
    require(storage["first_vertex"] == expected_first and digest(raw + extra) == storage["sha256"] and
            digest(extra) == storage["remaining_sha256"], "remaining range and bytes")
    require(isinstance(extra_ids, list) and len(extra_ids) == (total - selected if "vertex_identity" in data else 0),
            "remaining identity size")
    previous = selection["instance"]
    for identity in extra_ids:
        require(len(identity) == 3 and all(type(v) is int for v in identity) and
                selection["instance"] < identity[0] < selection["instance_count"] and identity[0] >= previous,
                "remaining instance identity")
        previous = identity[0]
    return raw + extra, identities + extra_ids


def validate_stage(folder, label, event, instance, vendor, worker_hash, stage, position_channels=False):
    result, job = load(folder / "result.json"), load(folder / "job.json")
    data, context = result["data"], result["context"]
    require(result["ok"] is True and result["schema"] == job["schema"] == 1, "schema")
    require(result["request_id"] == job["request_id"], "request identity")
    require(context["action"] == job["action"] == "mesh", "action identity")
    for key in ("options", "backend", "backend_sha256", "frame_sha256", "experiment_sha256"):
        require(context[key] == job[key], "context identity: " + key)
    require(context["frame_sha256"] == CAPTURES[label][1], "pinned capture")
    require(context["backend_sha256"] == worker_hash and context["experiment_sha256"] is None, "backend")
    require(digest(json.dumps(context, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()) ==
            result["context_id"], "context hash")
    options = context["options"]
    require(options["gpa_event"] == data["event"]["id"] == event, "event selection")
    require(options["instance"] == data["instance_selection"]["instance"] == instance, "instance selection")
    require(options["stage"] == data["requested_stage"] == stage, "stage selection")
    require(context["adapter"]["vendor"] == (vendor or 5140) and
            context["adapter"]["warp"] is (vendor == 0), "actual adapter")
    require(options.get("warp", False) is (vendor == 0) and
            (not vendor or options["gpu_vendor"] == vendor), "requested adapter")
    require(data["executed_stages"] == ["vs"] and data["pre_raster_uav_isolation"]["stages"] == [],
            "comparison requires VS-only geometry without pre-raster UAV effects")
    validate_modules(result["execution"]["loaded_modules"])
    for name, entry in result["artifacts"].items():
        require(Path(name).name == name, "artifact name")
        raw = (folder / name).read_bytes()
        require(len(raw) == entry["bytes"] and digest(raw) == entry["sha256"], "artifact: " + name)
    raw = (folder / "vertices.bin").read_bytes()
    count, stride = data["vertices"], data["stride"]
    require(count > 0 and stride > 0 and len(raw) == count * stride and digest(raw) == data["sha256"],
            "raw vertex storage")
    attribute = next(a for a in data["attributes"] if a["system_value"] == 1 and a["component_type"] == 3)
    require(attribute["components"] == [0, 1, 2, 3] and attribute["offset"] + 16 <= stride, "position layout")
    presentation = load(folder / "presentation.json")
    remaining = options.get("vertex_storage", "selected") == "remaining"
    require(remaining == ("vertex_storage" in data), "requested vertex storage scope")
    storage_raw, storage_ids = presentation_storage(folder, data, raw)
    stored_count = len(storage_raw) // stride
    if remaining:
        require(presentation["vertex_storage"] == data["vertex_storage"], "presentation storage provenance")
    mapping = presentation.get("replay_mesh")
    if position_channels:
        require(mapping is not None, "missing native semantic position mapping")
    sources, references = list(range(stored_count)), list(range(count))
    if mapping is not None:
        if "vertex_identity" in data:
            sources, all_references = compact_sources(data, storage_raw, storage_ids, stored_count)
            references = all_references[:count]
        expected_mapping = {"schema": 1, "strategy": "sorted_input_identities_and_output_variants"
                            if "vertex_identity" in data else "expanded_outputs", "vertex_sources": sources,
                            "reference_vertices": references, "vertex_stride": stride, "index_count": count}
        if position_channels:
            expected_mapping.update(position_components=[0, 1, 2, 3], obj_unavailable_reason=None)
        require(mapping == expected_mapping, "independent compact identity mapping")
        require((folder / "post_vertices.bin").read_bytes() ==
                b"".join(storage_raw[i * stride:(i + 1) * stride] for i in sources), "compact vertex records")
    positions = [struct.unpack_from("<4f", storage_raw, i * stride + attribute["offset"]) for i in sources]
    rows = list(csv.reader(io.StringIO((folder / "post_vertices.csv").read_text(encoding="utf-8"))))
    require(rows[0] == ["vertex", "x", "y", "z", "w"] and len(rows) == len(sources) + 1, "CSV layout")
    for i, (row, position) in enumerate(zip(rows[1:], positions)):
        require(int(row[0]) == i and len(row) == 5 and
                all(same_float(float(a), b) for a, b in zip(row[1:], position)), "CSV position value")
    obj = [line.split() for line in (folder / "post_geometry.obj").read_text(encoding="utf-8").splitlines()]
    obj_positions = [row[1:] for row in obj if row and row[0] == "v"]
    require(len(obj_positions) == len(sources), "OBJ position count")
    for row, position in zip(obj_positions, positions):
        w = position[3] or 1
        require(len(row) == 3 and all(same_float(float(a), b / w) for a, b in zip(row, position)),
                "OBJ perspective divide")
    faces = [[int(v) - 1 for v in row[1:]] for row in obj if row and row[0] == "f"]
    require(all(len(face) == 3 and all(0 <= v < len(sources) for v in face) for face in faces), "face bounds")
    selected_positions = [struct.unpack_from("<4f", raw, i * stride + attribute["offset"]) for i in range(count)]
    invalid_clip = any(p[3] == 0 or not all(map(math.isfinite, p)) for p in selected_positions)
    if invalid_clip:
        require(presentation["obj_unavailable_reason"] ==
                "Nonfinite clip position or zero W; raw attributes remain available", "explicit preview limit")
        require(all(presentation["mesh"][key] == [] for key in ("positions", "faces", "lines", "points")),
                "unavailable canonical preview")
    else:
        require(presentation["obj_unavailable_reason"] is None and
                [[references[v - 1] for v in face] for face in presentation["mesh"]["faces"]] == faces,
                "presentation faces")
    if stage == "vs-inputs":
        for extension, name in (("csv", "post_vertices.csv"), ("obj", "post_geometry.obj")):
            require((folder / ("ui-export." + extension)).read_bytes() == (folder / name).read_bytes(),
                    "UI export " + extension)
        require(load(folder / "ui-export.json")["presentation"] == presentation, "UI JSON export")
    return data, raw, faces, presentation, result["execution"]


def compare(inputs, final, instance):
    data, raw, faces, presentation, _ = inputs
    other, actual, final_faces, _, _ = final
    require(data["attributes"] == other["attributes"] and data["stride"] == other["stride"], "attribute layout")
    require(data["shader_resource"] == other["shader_resource"], "shader identity")
    require(data["instance_selection"]["original_parameters"] ==
            other["instance_selection"]["original_parameters"], "original draw parameters")
    require(data["input_topology"] == other["input_topology"] == 4, "current corpus requires triangle lists")
    references = presentation["tables"]["input_references"]["rows"]
    require(len(references) == len(data["input_assembly"]["references"]), "reference count")
    vertices = []
    for i, row in enumerate(references):
        require(row[:2] == [instance, i] and row[3] is False and row[4] == i, "reference ordering")
        require(row[2] == data["input_assembly"]["references"][i], "vertex identity")
        vertices.append(row[4])
    expected_faces = [vertices[i:i + 3] for i in range(0, len(vertices) - 2, 3)]
    mapping = presentation.get("replay_mesh", {"reference_vertices": list(range(data["vertices"]))})
    require(faces == [[mapping["reference_vertices"][v] for v in face] for face in expected_faces],
            "independent triangle-list reconstruction")
    stride = data["stride"]
    expected = b"".join(raw[v * stride:(v + 1) * stride] for face in expected_faces for v in face)
    differing = sum(a != b for a, b in zip(expected, actual)) + abs(len(expected) - len(actual))
    require(expected == actual, f"assembled VS and final SO differ in {differing} bytes")
    require(other["primitives"] == len(expected_faces) and other["vertices_per_primitive"] == 3,
            "final primitive count")
    require(final_faces == [[i, i + 1, i + 2] for i in range(0, other["vertices"], 3)], "final topology")
    return {"input_vertices": data["vertices"], "final_vertices": other["vertices"],
            "triangles": len(expected_faces), "compared_bytes": len(expected),
            "sha256": digest(expected), "parameters": data["instance_selection"]["original_parameters"]}


def compare_remaining(inputs, final, input_storage, final_storage, instance):
    data, raw, _, presentation, _ = inputs
    other, actual, _, _, _ = final
    stride = data["stride"]
    count = data["instance_selection"]["instance_count"] - instance
    require(count > 0 and len(input_storage) == len(raw) * count and
            len(final_storage) == len(actual) * count, "remaining instance ranges")
    for metadata, storage in ((data, input_storage), (other, final_storage)):
        require(metadata["vertex_storage"]["vertices"] * stride == len(storage) and
                metadata["vertex_storage"]["sha256"] == digest(storage), "complete remaining storage identity")
    rows = presentation["tables"]["input_references"]["rows"]
    references = [row[4] for row in rows[:len(rows) // 3 * 3]]
    expected = b"".join(input_storage[offset * len(raw) + index * stride:
                                      offset * len(raw) + (index + 1) * stride]
                        for offset in range(count) for index in references)
    require(expected == final_storage, "remaining assembled outputs differ from final stream output")
    return {"instances": count, "input_bytes": len(input_storage), "final_bytes": len(final_storage),
            "assembled_sha256": digest(expected)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--qt-bin", type=Path, required=True)
    parser.add_argument("--captures", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--validate-only", action="store_true", help="Recheck retained output without GPU execution")
    parser.add_argument("--vertex-storage", choices=("selected", "remaining"), default="selected")
    parser.add_argument("--position-channels", action="store_true", help="Require semantic position metadata added by the channel presentation implementation")
    parser.add_argument("--case-set", choices=CASE_SETS, default="baseline")
    args = parser.parse_args()
    cases = selected_cases(args.case_set)
    if args.validate_only:
        require(args.out.is_dir(), "Missing retained evidence directory")
        summary_path = args.out / "revalidation.json"
        require(not summary_path.exists(), "Revalidation report must be new")
    else:
        args.out.mkdir(parents=True, exist_ok=False)
        summary_path = args.out / "validation.json"
    for filename, sha in CAPTURES.values():
        with (args.captures / filename).open("rb") as capture:
            require(hashlib.file_digest(capture, "sha256").hexdigest() == sha, "capture hash: " + filename)
    worker_hash = digest((args.probe.parent / "FloraGPA.Worker.exe").read_bytes())
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    env["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + env["PATH"]
    records = []
    for vendor in (4318, 32902, 0):
        for label, event, instance in cases:
            name = f"{vendor}-{label}-{event}-{instance}"
            folder = args.out / name
            command = [str(args.probe.resolve()), str((args.captures / CAPTURES[label][0]).resolve()),
                       str(event), str(instance), str(vendor), str(folder.resolve())]
            if args.vertex_storage == "remaining":
                command.append("remaining")
            if args.validate_only:
                status = load(args.out / "validation.json")
                code = next(r["exit_code"] for r in status["cases"] if r["case"] == name)
                require(status["probe_sha256"] == digest(args.probe.read_bytes()), "Retained probe identity")
            else:
                run = subprocess.run(command, env=env, capture_output=True, timeout=420)
                (args.out / (name + ".log")).write_bytes(run.stdout + run.stderr)
                code = run.returncode
            record = {"case": name, "exit_code": code, "passed": False}
            try:
                require(code == 0, "probe failed; inspect retained log")
                probe = load(folder / "probe.json")
                require(probe["completed"] is True and probe["event"] == event and
                        probe["instance"] == instance and probe["vendor"] == vendor, "probe selections")
                validate_modules(probe["process"]["loaded_modules"])
                require(probe["elapsed_ms"] > 0 and probe["process"]["peak_working_set_bytes"] > 0,
                        "probe execution measurements")
                require([r["stage"] for r in probe["records"]] == ["vs-inputs", "final"] and
                        [r["ui_presented"] for r in probe["records"]] == [True, False], "UI presentation scope")
                require(all(load(folder / stage / "job.json")["options"].get("vertex_storage", "selected") ==
                            args.vertex_storage for stage in ("vs-inputs", "final")), "requested storage mode")
                pair = [validate_stage(folder / stage, label, event, instance, vendor, worker_hash, stage, args.position_channels)
                        for stage in ("vs-inputs", "final")]
                for snapshot, part in zip(probe["records"], pair):
                    context = load(folder / snapshot["stage"] / "result.json")["context_id"]
                    vertex_count = len(part[3]["replay_mesh"]["vertex_sources"]) if "replay_mesh" in part[3] else part[0]["vertices"]
                    require(snapshot["context_id"] == context and snapshot["vertices"] == vertex_count and
                            snapshot["faces"] == len(part[2]) and snapshot["elapsed_ms"] > 0,
                            "probe result identity and presentation counts")
                record.update(compare(*pair, instance))
                if args.vertex_storage == "remaining":
                    input_storage = presentation_storage(folder / "vs-inputs", pair[0][0], pair[0][1])[0]
                    final_storage = presentation_storage(folder / "final", pair[1][0], pair[1][1])[0]
                    record["remaining"] = compare_remaining(*pair, input_storage, final_storage, instance)
                    remaining_controls = 0
                    for target in ("input_storage", "final_storage", "count"):
                        changed, first, second = copy.deepcopy(pair), input_storage, final_storage
                        if target == "input_storage":
                            first = first[:-1] + bytes([first[-1] ^ 1])
                        elif target == "final_storage":
                            second = second[:-1] + bytes([second[-1] ^ 1])
                        else:
                            changed[0][0]["vertex_storage"]["vertices"] += 1
                        try:
                            compare_remaining(*changed, first, second, instance)
                        except ValueError:
                            remaining_controls += 1
                        else:
                            raise ValueError("Accepted corrupt remaining " + target)
                    record["remaining_corruption_controls"] = remaining_controls
                controls = 0
                for target in ("raw", "reference", "topology", "face"):
                    changed = copy.deepcopy(pair)
                    if target == "raw":
                        part = list(changed[1])
                        part[1] = bytes([part[1][0] ^ 1]) + part[1][1:]
                        changed[1] = tuple(part)
                    elif target == "reference":
                        changed[0][3]["tables"]["input_references"]["rows"][0][2] = -1
                    elif target == "topology":
                        changed[1][0]["input_topology"] = 5
                    else:
                        changed[1][2][0][0] = 99999999
                    try:
                        compare(*changed, instance)
                    except ValueError:
                        controls += 1
                    else:
                        raise ValueError("Accepted corrupt " + target)
                record.update(passed=True, corruption_controls=controls, probe_elapsed_ms=probe["elapsed_ms"],
                              probe_peak_working_set_bytes=probe["process"]["peak_working_set_bytes"],
                              execution=[{k: p[4][k] for k in ("elapsed_ms", "peak_working_set_bytes")}
                                         for p in pair])
            except (ValueError, KeyError, IndexError, StopIteration) as error:
                record["error"] = str(error)
            records.append(record)
            print(name, "PASS" if record["passed"] else record["error"], flush=True)
            summary = {"schema": 1, "completed": len(records) == len(cases) * 3,
                       "passed": all(r["passed"] for r in records), "case_set": args.case_set,
                       "position_channels": args.position_channels, "vertex_storage": args.vertex_storage,
                       "worker_sha256": worker_hash,
                       "probe_sha256": digest(args.probe.read_bytes()), "cases": records,
                       "renderdoc_parity": False, "full_mesh_parity": False}
            summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return 0 if all(r["passed"] for r in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())
