"""Validate retained native Replay Mesh UI evidence without external analyzers."""
import argparse
import copy
import csv
import hashlib
import io
import json
import math
import re
import struct
from pathlib import Path


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def same_float(a, b):
    return a == b and (a != 0 or math.copysign(1, a) == math.copysign(1, b))


def validate(name, job, result, files):
    vendor, stage = name.split("-")
    vendor = int(vendor)
    expected_event = {"vs": 1000, "gs": 150, "ds": 200}[stage]
    expected_stage = "vs-inputs" if stage == "vs" else "final"
    context, data = result["context"], result["data"]
    require(result["ok"] is True and result["schema"] == job["schema"] == 1, "result schema")
    require(job["action"] == context["action"] == "mesh", "action")
    require(result["request_id"] == job["request_id"], "request identity")
    for key in ("backend", "backend_sha256", "frame_sha256", "experiment_sha256", "options"):
        require(context[key] == job[key], "context " + key)
    canonical = json.dumps(context, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    require(digest(canonical) == result["context_id"], "context digest")
    options = context["options"]
    require(options["gpa_event"] == data["event"]["id"] == expected_event, "event")
    require(options["stage"] == data["requested_stage"] == expected_stage, "requested stage")
    require(data["shader_stage"] == stage, "executed stage")
    require(options["instance"] == data["instance_selection"]["instance"] == (1 if stage == "ds" else 0),
            "instance identity")
    adapter = context["adapter"]
    require(adapter["warp"] is (vendor == 0), "adapter class")
    require(adapter["vendor"] == (5140 if vendor == 0 else vendor), "adapter vendor")
    modules = result["execution"]["loaded_modules"]
    require(bool(modules), "module evidence")
    for module in modules:
        path = module.lower().replace("\\", "/")
        require("renderdoc" not in path and "intelswtools/gpa" not in path, "external analyzer module")
    for filename, entry in result["artifacts"].items():
        require(Path(filename).name == filename, "artifact name")
        raw = files[filename]
        require(len(raw) == entry["bytes"] and digest(raw) == entry["sha256"], "artifact " + filename)
    raw = files["vertices.bin"]
    require(digest(raw) == data["sha256"], "vertex digest")
    position = next(a for a in data["attributes"] if a["system_value"] == 1 and a["component_type"] == 3)
    count, stride, offset = data["vertices"], data["stride"], position["offset"]
    components = position["components"]
    require(components == list(range(len(components))) and 1 <= len(components) <= 4, "position layout")
    require(offset + 4 * len(components) <= stride and len(raw) == count * stride, "position bounds")
    positions = [struct.unpack_from("<" + "f" * len(components), raw, i * stride + offset) for i in range(count)]
    rows = list(csv.reader(io.StringIO(files["post_vertices.csv"].decode("utf-8"))))
    require(rows[0] == ["vertex"] + list("xyzw"[:len(components)]) and len(rows) == count + 1, "CSV shape")
    for i, (row, expected) in enumerate(zip(rows[1:], positions)):
        require(int(row[0]) == i and len(row) == len(expected) + 1, "CSV identity")
        require(all(same_float(float(a), b) for a, b in zip(row[1:], expected)), "CSV raw position")
    obj = [line.split() for line in files["post_geometry.obj"].decode("utf-8").splitlines() if line]
    vertices = [row[1:] for row in obj if row[0] == "v"]
    require(len(vertices) == count, "OBJ vertex count")
    for row, p in zip(vertices, positions):
        w = p[3] if len(p) == 4 and p[3] != 0 else 1
        require(len(row) == min(3, len(p)) and
                all(same_float(float(a), b / w) for a, b in zip(row, p)), "OBJ divided position")
    faces = [list(map(int, row[1:])) for row in obj if row[0] == "f"]
    require(len(faces) == (2 if stage == "ds" else 1), "fixture face count")
    require(all(len(face) == 3 and all(1 <= index <= count for index in face) for face in faces), "face bounds")
    require(faces == [[i + 1, i + 2, i + 3] for i in range(0, count, 3)], "fixture topology")
    return {"case": name, "vertices": count, "faces": len(faces), "adapter": adapter,
            "elapsed_ms": result["execution"]["elapsed_ms"],
            "peak_working_set_bytes": result["execution"]["peak_working_set_bytes"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    expected = {f"{v}-{s}" for v in (0, 4318, 32902) for s in ("vs", "gs", "ds", "ds-uav")}
    require({p.name for p in args.directory.iterdir() if p.is_dir()} == expected, "complete matrix")
    require(re.search(r"Totals: 23 passed, 0 failed, 0 skipped, 0 blacklisted,",
                      args.log.read_text(encoding="utf-8")), "complete Qt suite")
    records, gates, controls = [], [], 0
    for name in sorted(expected):
        folder = args.directory / name
        job = read_json(folder / "job.json")
        if name.endswith("-ds-uav"):
            gate = read_json(folder / "open-gate.json")
            require(gate == {"instance_selection_supported": False, "fallback": False}, "open gate")
            require("Instance boundaries through HS/DS/GS with UAV access" in
                    (folder / "worker.log").read_text(encoding="utf-8"), "specific gate failure")
            require(job["options"]["instance"] == 1 and job["options"]["stage"] == "final", "gate selection")
            gates.append(name)
            continue
        result = read_json(folder / "result.json")
        files = {p.name: p.read_bytes() for p in folder.iterdir() if p.is_file()}
        records.append(validate(name, job, result, files))
        for target in ("request", "module", "vertex", "csv", "obj"):
            altered, changed = copy.deepcopy(result), dict(files)
            if target == "request":
                altered["request_id"] = "stale"
            elif target == "module":
                altered["execution"]["loaded_modules"].append("C:/RenderDoc/renderdoc.dll")
            elif target == "vertex":
                changed["vertices.bin"] = bytes([changed["vertices.bin"][0] ^ 1]) + changed["vertices.bin"][1:]
            elif target == "csv":
                changed["post_vertices.csv"] = b"vertex,x,y,z,w\n0,42,0,0,1\n"
            else:
                changed["post_geometry.obj"] += b"f 999 998 997\n"
            try:
                validate(name, job, altered, changed)
            except ValueError:
                controls += 1
            else:
                raise ValueError("Accepted corrupt " + target + " in " + name)
    paths = sorted(p for p in args.directory.rglob("*") if p.is_file()) + [args.log]
    summary = {"schema": 1, "status": "ok", "successful_cases": records, "open_gate_cases": gates,
               "corruption_controls": controls, "default_cutover": False, "full_renderdoc_removal": False,
               "evidence": [{"path": p.as_posix(), "bytes": p.stat().st_size,
                             "sha256": digest(p.read_bytes())} for p in paths]}
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(summary, output, indent=2)
        output.write("\n")
    print(json.dumps({"successful_cases": len(records), "open_gate_cases": len(gates),
                      "corruption_controls": controls}))


if __name__ == "__main__":
    main()
