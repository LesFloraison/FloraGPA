"""Validate private pixel records against saved MRT observations and geometry."""
import argparse
import copy
import json
from pathlib import Path
import struct

from validate_pixel_geometry import identity, require, verify as verify_geometry
from validate_pixel_input_capture import MODES


def verify(meta, raw, targets=None):
    require(meta["backend"] in ("dxbc-pixel-raster-inputs/1", "dxbc-pixel-raster-inputs/2"), "Unknown raster backend")
    require(meta["scope"] == "isolated_primitive_pre_depth_inputs"
            and meta["original_fragment_eligibility"] is False, "Incorrect observation scope")
    capacity = meta["max_records"]
    require(1 <= capacity <= 4096 and meta["record_stride"] == 144, "Invalid record capacity/stride")
    require(len(raw) == 16 + 144 * capacity, "Record storage size differs")
    count, a, b, c = struct.unpack_from("<4I", raw)
    require(count == meta["records"] and count <= capacity, "Record count/overflow differs")
    require((a, b, c) == (0, 0, 0), "Reserved header was written")
    require(not any(raw[16 + count * 144:]), "Unallocated record storage was written")
    x, y, sample, layer = (meta[k] for k in ("x", "y", "sample", "layer"))
    require(0 <= sample < meta["raster_samples"] <= 32, "Invalid raster sample selection")
    require(meta["valid_mask"] in range(1, 16), "Invalid input component validity")
    require(meta["private_scissor_guard"] == [x - 1, y - 1, x + 2, y + 2], "Invalid quad guard")
    require(meta["geometry"]["input_register"] == meta["input_register"], "Input/geometry register differs")
    for i in range(count):
        words = list(struct.iter_unpack("<4I", raw[16 + i * 144:16 + (i + 1) * 144]))
        require(words[4][:2] == (x, y) and all(v in (0, 1) for v in words[4][2:]), "Invalid pixel/quad role")
        rx, ry = words[4][2:]
        require(words[8][:2] == (layer, sample), "Invalid record layer/sample")
        if meta["backend"] == "dxbc-pixel-raster-inputs/1":
            require(words[8][2:] == (0, 0), "Legacy reserved record words were written")
        else:
            require(words[8][3] in (0, 1), "Invalid observed front-face Boolean")
        direction = struct.unpack("<4f", struct.pack("<4I", *words[7]))
        require(direction == (-1.0 if rx else 1.0, -1.0 if ry else 1.0, 0.0, 0.0),
                "Invalid quad orientation")
        lane = rx + ry * 2
        require(words[6][lane] & (1 << sample), "Selected raster sample lacks coverage")
        require(words[5][lane] == (sample if meta["sample_frequency"] else 0), "Sample invocation differs")
        if targets is not None:
            target = meta["raster_state"]["target"]
            address = ((y * target["width"] + x) * target["samples"] + sample) * 16
            for n in range(8):
                require(words[n] == struct.unpack_from("<4I", targets[n], address),
                        f"Private record differs from full MRT observation {n}")
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("synthetic", type=Path)
    parser.add_argument("real", nargs="+", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    expected = {f"{v}-{m.replace(' ', '-')}-{s}" for v in (0, 4318, 32902)
                for m in MODES for s in (1, 4)}
    cases = sorted(p.parent for p in args.synthetic.glob("*/records.json"))
    require({p.name for p in cases} == expected, "Incomplete synthetic raster matrix")
    runs, files = [], set()
    witness = None
    for case in cases:
        meta = json.loads((case / "records.json").read_text())
        fixture = json.loads((case / "report.json").read_text())
        require(meta["adapter_luid"] == fixture["adapter_luid"], "Synthetic raster adapter changed")
        require(meta["input_register"] == fixture["register"] and meta["valid_mask"] == fixture["mask"],
                "Synthetic input identity differs")
        require(meta["records"] == 1, "Synthetic raster record is empty/ambiguous")
        raw = (case / "records.bin").read_bytes()
        targets = [(case / f"target-{i}.bin").read_bytes() for i in range(8)]
        count = verify(meta, raw, targets)
        runs.append({"path": str(case), "kind": "synthetic", "records": count, "exact_components": count * 32})
        files.update(p for p in case.iterdir() if p.is_file())
        if case.name == "0-linear-1":
            witness = (meta, raw, targets)
    real_frames = set()
    for directory in args.real:
        report = json.loads((directory / "report.json").read_text())
        require(report["status"] == "ok" and report["raster_status"] == "observed" and report["cases"],
                "Real raster probe did not observe inputs")
        real_frames.add((report["frame_sha256"], report["adapter_vendor"]))
        source = json.loads((directory / "source-geometry.json").read_text())
        source_bytes = (directory / "source-geometry.bin").read_bytes()
        for case in report["cases"]:
            path = directory / f"input-{case['register']}"
            meta = json.loads((path / "records.json").read_text())
            require(meta["adapter_luid"] == report["adapter_luid"], "Real raster adapter changed")
            require(meta["raster_state"] == report["raster_state"], "Real raster state differs")
            require([meta[k] for k in ("x", "y", "sample", "layer")] == report["selection"],
                    "Real raster selection differs")
            require(meta["input_register"] == case["register"] and meta["records"] == case["input_records"],
                    "Real raster input/count differs")
            require(meta["geometry"]["primitive_ordinal"] == report["primitive_ordinal"],
                    "Real primitive selection differs")
            verify_geometry(source, source_bytes, meta["geometry"], (path / "record-vertices.bin").read_bytes())
            count = verify(meta, (path / "records.bin").read_bytes())
            require(count > 0, "Real register observation is empty")
            runs.append({"path": str(path), "kind": "real", "records": count,
                         "exact_components": 0, "original_input_oracle": False})
        files.update(p for p in directory.rglob("*") if p.is_file())
    frame_hashes = {frame for frame, vendor in real_frames}
    require(len(frame_hashes) == 3 and real_frames == {
        (frame, vendor) for frame in frame_hashes for vendor in (4318, 32902, 5140)},
        "Expected three captures on all three adapters")
    controls = {}
    meta, raw, targets = witness
    overflow_count = meta["max_records"] + 1
    changes = {"overflow": (0, overflow_count), "reserved_header": (4, 1),
               "pixel": (16 + 64, meta["x"] + 1), "orientation": (16 + 112, 0),
               "layer": (16 + 128, meta["layer"] + 1), "unused_record": (16 + 144, 1),
               "input_bits": (16, struct.unpack_from("<I", raw, 16)[0] ^ 1)}
    for name, (offset, value) in changes.items():
        changed = bytearray(raw)
        struct.pack_into("<I", changed, offset, value)
        changed_meta = copy.deepcopy(meta)
        if name == "overflow":
            changed_meta["records"] = overflow_count
        try:
            verify(changed_meta, changed, targets)
        except ValueError as error:
            controls[name] = str(error)
        else:
            raise ValueError(f"Corruption was accepted: {name}")
    result = {"schema": 1, "synthetic_cases": len(cases), "real_registers": len(runs) - len(cases),
              "real_capture_adapters": len(real_frames), "records": sum(r["records"] for r in runs),
              "exact_mrt_components": sum(r["exact_components"] for r in runs),
              "negative_controls": controls, "runs": runs, "artifacts": [identity(p) for p in sorted(files)]}
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps({k: v for k, v in result.items() if k not in ("runs", "artifacts")}, indent=2))


if __name__ == "__main__":
    main()
