"""Verify retained native storage, synthetic shader results and saved migration exports."""
import argparse
import csv
import io
import json
from pathlib import Path
import re
import shutil
import struct
import tempfile

from compare_native_mesh_baseline import compare as compare_baseline
from validate_native_mesh_captures import (
    compare, compare_remaining, digest, load, presentation_storage, require,
    selected_cases, validate_modules, validate_stage,
)


def synthetic(folder, vendor, stage, instance):
    job, result = load(folder / "job.json"), load(folder / "result.json")
    data, context = result["data"], result["context"]
    require(result["ok"] is True and result["backend"] == "dx11-native/10" and
            result["request_id"] == job["request_id"], "synthetic result identity")
    for key in ("schema", "backend", "backend_sha256", "frame_sha256", "experiment_sha256", "action", "options"):
        require(context[key] == job[key], "synthetic context: " + key)
    require(result["context_id"] == digest(json.dumps(context, sort_keys=True, separators=(",", ":"),
                                                      ensure_ascii=False).encode()), "context digest")
    options = job["options"]
    require(job["action"] == "mesh" and options["gpa_event"] == 200 and options["instance"] == instance and
            options["stage"] == ("final" if stage == "gs" else stage) and
            options["vertex_storage"] == "remaining", "synthetic selection")
    require(context["adapter"]["vendor"] == (vendor or 5140) and
            context["adapter"]["warp"] == (vendor == 0), "synthetic adapter")
    validate_modules(result["execution"]["loaded_modules"])
    for name, entry in result["artifacts"].items():
        require(Path(name).name == name, "artifact name")
        raw = (folder / name).read_bytes()
        require(len(raw) == entry["bytes"] and digest(raw) == entry["sha256"], "artifact digest: " + name)
    raw = (folder / "vertices.bin").read_bytes()
    require(digest(raw) == data["sha256"] and data["stride"] == 16, "selected vertex bytes")
    storage, identities = presentation_storage(folder, data, raw)
    selected = (instance + 1) * 3 if stage == "gs" else 6
    limit = 3 if stage == "gs" else 2
    total = sum((owner + 1) * 3 if stage == "gs" else 6 for owner in range(instance, limit))
    require(len(raw) == selected * 16 and len(storage) == total * 16, "synthetic storage boundaries")
    values = list(struct.iter_unpack("<4f", storage))
    offset = 0
    for owner in range(instance, limit):
        count = (owner + 1) * 3 if stage == "gs" else 6
        actual = values[offset:offset + count]
        if stage == "final":
            for patch in range(2):
                expected = {(float(patch * 3 + i), float(owner), patch + i * .25, 1.) for i in range(3)}
                require(set(actual[patch * 3:patch * 3 + 3]) == expected, "terminal DS patch vertices")
        else:
            require(actual == [(float(i), float(owner), 0., 1.) for i in range(count)], "VS or GS shader output")
        offset += count
    if identities:
        require(identities == [[owner, i, i] for owner in range(instance, limit) for i in range(6)],
                "synthetic input identities")
    presentation = load(folder / "presentation.json")
    mapping = presentation["replay_mesh"]
    require(presentation["vertex_storage"] == data["vertex_storage"] and
            mapping["vertex_sources"] == list(range(total)) and
            mapping["reference_vertices"] == list(range(selected)) and mapping["index_count"] == selected,
            "stored vertices and selected references")
    require((folder / "post_vertices.bin").read_bytes() == storage, "complete presented vertex storage")
    rows = list(csv.reader(io.StringIO((folder / "post_vertices.csv").read_text(encoding="utf-8"))))
    require(rows[0] == ["vertex", "x", "y", "z", "w"] and len(rows) == total + 1, "CSV rows")
    for index, (row, value) in enumerate(zip(rows[1:], values, strict=True)):
        require(int(row[0]) == index and tuple(map(float, row[1:])) == value, "CSV values")
    obj = [line.split() for line in (folder / "post_geometry.obj").read_text(encoding="utf-8").splitlines()]
    require([tuple(map(float, row[1:])) for row in obj if row and row[0] == "v"] ==
            [value[:3] for value in values], "OBJ stored positions")
    require([[int(v) - 1 for v in row[1:]] for row in obj if row and row[0] == "f"] ==
            [[i, i + 1, i + 2] for i in range(0, selected, 3)], "OBJ selected faces")
    return {"case": folder.name, "selected_vertices": selected, "stored_vertices": total,
            "storage_sha256": digest(storage), "context_id": result["context_id"],
            "adapter": context["adapter"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("synthetic", "native", "baseline", "logs", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--cli", type=Path, help="Retained BF1/1415/1 CLI job using remaining storage")
    args = parser.parse_args()
    require(not args.out.exists(), "Verification report must be new")
    cases = [(vendor, stage, instance) for vendor in (0, 4318, 32902)
             for stage in ("vs-inputs", "vs-index", "final", "gs")
             for instance in range(3 if stage == "gs" else 2)]
    require({p.name for p in args.synthetic.iterdir() if p.is_dir()} ==
            {f"{v}-{s}-{i}" for v, s, i in cases}, "27 synthetic selections")
    synthetic_records = [synthetic(args.synthetic / f"{v}-{s}-{i}", v, s, i) for v, s, i in cases]
    controls = []
    selected_folder = args.synthetic / "0-vs-inputs-0"
    selected_raw = (selected_folder / "vertices.bin").read_bytes()
    for offset in (-1, 1):
        selected_data = load(selected_folder / "result.json")["data"]
        selected_data.pop("vertex_storage")
        presentation_storage(selected_folder, selected_data, selected_raw)
        selected_data["vertices"] += offset
        try:
            presentation_storage(selected_folder, selected_data, selected_raw)
        except ValueError:
            controls.append("selected_count_" + str(offset))
        else:
            raise ValueError("Accepted corrupt selected storage count")
    for target in ("remaining_bytes", "remaining_identity", "first_vertex", "full_digest", "CSV", "OBJ", "context"):
        with tempfile.TemporaryDirectory(prefix="flora-mesh-storage-") as temporary:
            folder = Path(temporary) / "case"
            shutil.copytree(args.synthetic / "0-vs-inputs-0", folder)
            if target == "remaining_bytes":
                path = folder / "remaining_vertices.bin"
                raw = path.read_bytes()
                path.write_bytes(bytes([raw[0] ^ 1]) + raw[1:])
            elif target == "remaining_identity":
                path = folder / "remaining_identities.json"
                value = load(path)
                value[0][0] = 0
                path.write_text(json.dumps(value), encoding="utf-8")
            elif target in ("first_vertex", "full_digest", "context"):
                path = folder / "result.json"
                value = load(path)
                if target == "first_vertex":
                    value["data"]["vertex_storage"]["first_vertex"] += 1
                elif target == "full_digest":
                    value["data"]["vertex_storage"]["full_sha256"] = "0" * 64
                    value["data"]["instance_selection"]["full_capture"]["sha256"] = "0" * 64
                else:
                    value["context"]["frame_sha256"] = "changed"
                path.write_text(json.dumps(value), encoding="utf-8")
            else:
                path = folder / ("post_vertices.csv" if target == "CSV" else "post_geometry.obj")
                path.write_text(path.read_text(encoding="utf-8").replace("1", "9"), encoding="utf-8")
            if target in ("remaining_bytes", "remaining_identity"):
                result = load(folder / "result.json")
                raw = path.read_bytes()
                result["artifacts"][path.name] = {"bytes": len(raw), "sha256": digest(raw)}
                (folder / "result.json").write_text(json.dumps(result), encoding="utf-8")
            try:
                synthetic(folder, 0, "vs-inputs", 0)
            except (ValueError, KeyError):
                controls.append(target)
            else:
                raise ValueError("Accepted corrupt " + target)
    validation = load(args.native / "validation.json")
    require(validation["completed"] and validation["passed"] and validation["case_set"] == "all" and
            validation["vertex_storage"] == "remaining" and len(validation["cases"]) == 30,
            "complete real-capture matrix")
    expected = {f"{v}-{a}-{b}-{c}" for v in (4318, 32902, 0) for a, b, c in selected_cases("all")}
    require({c["case"] for c in validation["cases"]} == expected and
            all(c["corruption_controls"] == 4 and c["remaining_corruption_controls"] == 3
                for c in validation["cases"]), "real-capture selections and controls")
    for vendor in (4318, 32902, 0):
        for label, event, instance in selected_cases("all"):
            root = args.native / f"{vendor}-{label}-{event}-{instance}"
            pair = [validate_stage(root / stage, label, event, instance, vendor,
                                   validation["worker_sha256"], stage, True) for stage in ("vs-inputs", "final")]
            compare(*pair, instance)
            storage = [presentation_storage(root / stage, part[0], part[1])[0]
                       for stage, part in zip(("vs-inputs", "final"), pair, strict=True)]
            compare_remaining(*pair, *storage, instance)
    baselines = [compare_baseline(args.baseline, args.native, *case) for case in selected_cases("all")]
    require(all(c["strict_exports_equal"] and c["indices_equal"] and c["referenced_records_equal"]
                for c in baselines), "saved baseline export parity")
    cli = None
    if args.cli:
        reference = load(args.native / "4318-bf1-1415-1/vs-inputs/result.json")
        result = load(args.cli / "result.json")
        job = load(args.cli / "job.json")
        require(result["ok"] is True and result["request_id"] == job["request_id"] and
                result["context"] == reference["context"] and result["context_id"] == reference["context_id"] and
                result["data"] == reference["data"] and result["artifacts"] == reference["artifacts"],
                "CLI context and canonical results")
        validate_modules(result["execution"]["loaded_modules"])
        for name, entry in result["artifacts"].items():
            require(Path(name).name == name, "CLI artifact name")
            raw = (args.cli / name).read_bytes()
            require(len(raw) == entry["bytes"] and digest(raw) == entry["sha256"], "CLI artifact identity")
        cli = {"context_id": result["context_id"], "canonical_artifacts": len(result["artifacts"]),
               "selected_vertices": result["data"]["vertices"],
               "stored_vertices": result["data"]["vertex_storage"]["vertices"]}
    regression = {}
    for suite, expected in {"NativeReplayMeshUi": (99, 0, 0), "NativeAnalysis": (49, 0, 0),
                            "PostTransform": (13, 0, 0), "NativeComputeDebug": (68, 0, 0),
                            "GeometryUavDependency": (69, 0, 0), "ReplayMeshUi": (4, 0, 2)}.items():
        text = (args.logs / f"mesh-storage-{suite}-verified.log").read_text(encoding="utf-8")
        match = re.search(r"Totals: (\d+) passed, (\d+) failed, (\d+) skipped", text)
        require(match and tuple(map(int, match.groups())) == expected and "Finished testing" in text,
                "regression log: " + suite)
        regression[suite] = dict(zip(("passed", "failed", "skipped"), expected))
    args.out.write_text(json.dumps({"schema": 1, "completed": True, "passed": True,
                                   "synthetic": synthetic_records, "corruption_controls": controls,
                                   "native_cases": 30, "native_corruption_controls": 210,
                                   "baseline_exports": baselines, "cli": cli, "regression": regression}, indent=2) + "\n",
                        encoding="utf-8")
    print("27 synthetic selections, 30 real selections and 10 strict saved-baseline exports verified")


if __name__ == "__main__":
    main()
