"""Compare saved migration-baseline exports; never load an external analyzer."""
import argparse
import json
from pathlib import Path
from validate_native_mesh_captures import CASES, compact_sources, digest, load, require, validate_modules


def compare(baseline, native, label, event, instance):
    old = baseline / ("gf2-113" if label == "gf2" and event == 113 else f"{label}-{event}-{instance}")
    new = native / f"4318-{label}-{event}-{instance}" / "vs-inputs"
    reference, job = load(old / "result.json"), load(old / "job.json")
    result, request = load(new / "result.json"), load(new / "job.json")
    data = result["data"]
    require(reference["ok"] is True and result["ok"] is True, "completed results")
    require(job["gpa_event"] == reference["gpa_event"] == request["options"]["gpa_event"] == event,
            "matching GPA event")
    require(job["instance"] == request["options"]["instance"] == instance and
            job["stage"] == "VSOut" and request["options"]["stage"] == "vs-inputs", "matching selection")
    require(result["context"]["adapter"]["vendor"] == 4318 and not result["context"]["adapter"]["warp"],
            "native comparison adapter")
    environment = reference["replay_environment"]
    require(environment["gpu_vendor_name"] == "NVIDIA" and environment["degraded"] is False and
            environment["remote"] is False, "baseline replay environment")
    original = (new / "vertices.bin").read_bytes()
    require(digest(original) == data["sha256"], "native raw digest")
    sources, references = compact_sources(data, original, load(new / "identities.json"))
    stride = data["stride"]
    expected = b"".join(original[i * stride:(i + 1) * stride] for i in sources)
    actual = (new / "post_vertices.bin").read_bytes()
    require(actual == expected, "native compact record provenance")
    previous = (old / "post_vertices.bin").read_bytes()
    mesh = reference["mesh"]
    require(mesh["vertexByteStride"] == stride and len(previous) == reference["vertex_count"] * stride,
            "baseline vertex storage")
    index_data = (old / "post_indices.bin").read_bytes()
    width = mesh["indexByteStride"]
    require(width in (1, 2, 4) and len(index_data) == reference["index_count"] * width,
            "baseline index storage")
    indices = [int.from_bytes(index_data[i:i + width], "little") + mesh["baseVertex"]
               for i in range(0, len(index_data), width)]
    require(all(0 <= i < reference["vertex_count"] for i in indices), "baseline index bounds")
    require(len(indices) == len(references) and data["input_topology"] == 4, "comparable indexed triangle lists")
    expanded_old = b"".join(previous[i * stride:(i + 1) * stride] for i in indices)
    expanded_new = b"".join(actual[i * stride:(i + 1) * stride] for i in references)
    equality = {name: (old / name).read_bytes() == (new / name).read_bytes()
                for name in ("post_vertices.bin", "post_vertices.csv", "post_geometry.obj")}
    prefix_equal = previous[:len(actual)] == actual
    return {"case": f"{label}-{event}-{instance}", "gpa_event": event, "instance": instance,
            "strict_exports_equal": all(equality.values()),
            "export_equality": equality, "indices_equal": indices == references,
            "referenced_records_equal": expanded_old == expanded_new,
            "selected_record_prefix_equal": prefix_equal,
            "baseline_vertices": reference["vertex_count"], "native_vertices": len(sources),
            "baseline_trailing_vertices": (len(previous) - len(actual)) // stride if prefix_equal else None,
            "indices": len(indices), "referenced_bytes": len(expanded_new),
            "native_compact_sha256": digest(actual), "baseline_buffer_sha256": digest(previous),
            "baseline_capture": job["capture"], "native_frame_sha256": request["frame_sha256"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--all-instances", type=Path, help="Native jobs without an instance selection for trailing-data cases")
    args = parser.parse_args()
    records = [compare(args.baseline, args.native, *case) for case in CASES]
    if args.all_instances:
        for record in records:
            if not record["baseline_trailing_vertices"]:
                continue
            folder = args.all_instances / record["case"]
            result, job = load(folder / "result.json"), load(folder / "job.json")
            require(result["ok"] is True and "instance" not in job["options"] and
                    result["context"]["options"] == job["options"], "all-instance request")
            require(job["action"] == "mesh" and job["options"]["stage"] == "vs-inputs" and
                    job["options"]["gpa_event"] == record["gpa_event"], "all-instance selection")
            require(job["frame_sha256"] == record["native_frame_sha256"] and
                    result["context"]["adapter"]["vendor"] == 4318 and
                    result["context"]["adapter"]["warp"] is False, "all-instance frame and adapter")
            validate_modules(result["execution"]["loaded_modules"])
            for name, entry in result["artifacts"].items():
                require(Path(name).name == name, "artifact name")
                raw = (folder / name).read_bytes()
                require(len(raw) == entry["bytes"] and digest(raw) == entry["sha256"], "all-instance artifact")
            raw, data = (folder / "vertices.bin").read_bytes(), result["data"]
            require(digest(raw) == data["sha256"] and data["event"]["id"] == job["options"]["gpa_event"],
                    "all-instance output identity")
            sources, _ = compact_sources(data, raw, load(folder / "identities.json"))
            stride = data["stride"]
            compact = b"".join(raw[i * stride:(i + 1) * stride] for i in sources)
            record["all_instances"] = {"vertices": len(sources), "sha256": digest(compact),
                                       "matches_baseline_buffer": digest(compact) == record["baseline_buffer_sha256"]}
    summary = {"schema": 1, "completed": True,
               "strict_exports_equal": all(r["strict_exports_equal"] for r in records),
               "referenced_records_equal": all(r["referenced_records_equal"] for r in records),
               "indices_equal": all(r["indices_equal"] for r in records), "cases": records,
               "full_mesh_parity": False, "full_renderdoc_removal": False}
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(summary, output, indent=2)
        output.write("\n")
    for record in records:
        print(record["case"], "exact exports" if record["strict_exports_equal"] else "strict export difference",
              "referenced records:", record["referenced_records_equal"], flush=True)
    return 0 if summary["strict_exports_equal"] and summary["indices_equal"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
