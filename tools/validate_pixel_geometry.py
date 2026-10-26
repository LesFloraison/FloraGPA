"""Check saved pixel bridge mappings against source geometry and GPU observations."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import struct


def require(condition, message):
    if not condition:
        raise ValueError(message)


def identity(path):
    data = path.read_bytes()
    return {"path": str(path), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def verify(source, raw, bridge, payload, gpu=None):
    factor, stride = source["vertices_per_primitive"], source["stride"]
    ordinal, target_stride = bridge["primitive_ordinal"], bridge["stride"]
    require(source["geometry_stage"] == "final_pre_rasterization", "Wrong geometry scope")
    require(factor in (1, 2, 3) and bridge["vertices_per_primitive"] == factor, "Wrong primitive size")
    require(0 <= ordinal < source["primitives"] and len(raw) == source["vertices"] * stride,
            "Invalid source geometry size/selection")
    require(len(payload) == factor * target_stride, "Invalid bridge payload size")
    attrs = {(a["semantic"].lower(), a["index"]): a for a in source["attributes"]}
    require(len(attrs) == len(source["attributes"]), "Duplicate source semantic")
    raster = {key for key, a in attrs.items() if a["system_value"] in (1, 2, 3, 4, 5)}
    outputs = bridge["output_mapping"]
    require(raster <= {(f["semantic"].lower(), f["index"]) for f in outputs}, "Rasterizer output was lost")
    expected_gpu = []
    masks = {}
    for f in outputs:
        require(not masks.get(f["register"], 0) & f["mask"], "Overlapping bridge outputs")
        masks[f["register"]] = masks.get(f["register"], 0) | f["mask"]
    for vertex in range(factor):
        for f in outputs:
            key = (f["semantic"].lower(), f["index"])
            a = attrs.get(key)
            components = [c for c in range(4) if f["mask"] & (1 << c)]
            require(components and len(components) == len(f["components"]), "Invalid component mask")
            if a is not None:
                require((a["system_value"], a["component_type"]) == (f["system_value"], f["component_type"]),
                        "Producer/consumer types differ")
            for c, item in zip(components, f["components"]):
                semantic_component = c - components[0]
                require(item["output_component"] == c and item["semantic_component"] == semantic_component,
                        "Invalid component remapping")
                if a is not None:
                    require(semantic_component in a["components"], "Source semantic component is missing")
                    offset = a["offset"] + a["components"].index(semantic_component) * 4
                    require(item["source_byte_offset"] == offset, "Incorrect source byte offset")
                    require(f["source"] == "captured_final_geometry", "Incorrect captured-value provenance")
                    value = struct.unpack_from("<I", raw, (ordinal * factor + vertex) * stride + offset)[0]
                else:
                    require(f["system_value"] == 7 and f["source"] == "explicit_original_primitive_id"
                            and item["source_byte_offset"] is None, "Invented producer value")
                    value = bridge["original_primitive_id"]
                    require(type(value) is int and 0 <= value <= 0xffffffff, "Original primitive ID is unknown")
                position = vertex * target_stride + f["slot"] * 16 + c * 4
                require(position + 4 <= len(payload), "Payload mapping bounds")
                require(struct.unpack_from("<I", payload, position)[0] == value, "Bridge changed source bits")
                expected_gpu.append(value)
    if gpu is not None:
        require(len(gpu) == len(expected_gpu) * 4, "GPU roundtrip size differs")
        require([v[0] for v in struct.iter_unpack("<I", gpu)] == expected_gpu, "GPU roundtrip changed source bits")
    return len(expected_gpu)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", type=Path, nargs="+")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    cases, files, witness = [], set(), None
    for directory in args.directories:
        if (directory / "report.json").exists():
            report = json.loads((directory / "report.json").read_text())
            require(report["status"] == "ok" and report["cases"], "Real bridge probe did not complete")
            declared = set(map(int, re.findall(r"\bdcl_input_ps(?:_sgv|_siv)?\b[^\n]*\bv(\d+)\.",
                                            (directory / "original-ps.asm.txt").read_text())))
            require(declared == {c["register"] for c in report["cases"]}, "Real PS input register coverage differs")
            sources = [(directory / "source-geometry.json", directory / "source-geometry.bin",
                        directory / f"input-{c['register']}" / "bridge.json",
                        directory / f"input-{c['register']}" / "vertices.bin",
                        directory / f"input-{c['register']}" / "roundtrip.bin") for c in report["cases"]]
        else:
            sources = [(p / "source-geometry.json", p / "source-geometry.bin", p / "bridge-geometry.json",
                        p / "bridge-geometry.bin", None) for p in sorted(directory.iterdir()) if p.is_dir()]
        require(sources, "No geometry observations")
        for source_path, raw_path, bridge_path, payload_path, gpu_path in sources:
            values = (json.loads(source_path.read_text()), raw_path.read_bytes(),
                      json.loads(bridge_path.read_text()), payload_path.read_bytes(),
                      gpu_path.read_bytes() if gpu_path else None)
            count = verify(*values)
            cases.append({"bridge": str(bridge_path), "components": count, "gpu_roundtrip": gpu_path is not None})
            if gpu_path is not None:
                witness = values
        files.update(p for p in directory.rglob("*") if p.is_file())
    require(witness is not None, "Real geometry GPU roundtrip evidence is required")
    negatives = {}
    for label in ("source", "payload", "gpu", "offset", "component", "ordinal"):
        values = copy.deepcopy(list(witness))
        first = values[2]["output_mapping"][0]["components"][0]
        if label in ("source", "payload", "gpu"):
            index = {"source": 1, "payload": 3, "gpu": 4}[label]
            offset = 0
            if label == "source":
                offset = values[2]["primitive_ordinal"] * values[0]["vertices_per_primitive"] * values[0]["stride"] + first["source_byte_offset"]
            if label == "payload":
                offset = values[2]["output_mapping"][0]["slot"] * 16 + first["output_component"] * 4
            raw = bytearray(values[index]); raw[offset] ^= 1; values[index] = bytes(raw)
        elif label == "offset": first["source_byte_offset"] += 4
        elif label == "component": first["semantic_component"] ^= 1
        else: values[2]["primitive_ordinal"] = values[0]["primitives"]
        try:
            verify(*values)
        except ValueError as error:
            negatives[label] = str(error)
        else:
            raise ValueError(f"Accepted corrupt geometry evidence: {label}")
    report = {"schema": 1, "cases": cases, "components": sum(c["components"] for c in cases),
              "gpu_roundtrip_components": sum(c["components"] for c in cases if c["gpu_roundtrip"]),
              "negative_controls": negatives, "artifacts": [identity(p) for p in sorted(files)]}
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(report, output, indent=2); output.write("\n")
    print(json.dumps({k: v for k, v in report.items() if k not in ("cases", "artifacts")}, indent=2))


if __name__ == "__main__":
    main()
