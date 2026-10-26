"""Verify exported original-PS input transport observations without rerunning a GPU."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

MODES = ("linear", "noperspective", "centroid", "noperspective centroid", "sample",
         "noperspective sample", "nointerpolation", "uint", "sint", "sparse", "min16",
         "trigger", "position", "position-z", "sample-id", "sm4", "sm41")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def identity(path):
    data = path.read_bytes()
    return {"path": str(path), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def validate(directory, replacements=None):
    replacements = replacements or {}
    report = json.loads((directory / "report.json").read_text())
    samples, mode, mask = report["samples"], report["mode"], report["mask"]
    require(samples in (1, 4) and mode in MODES, "Invalid fixture mode")
    expected_mask = 4 if mode == "position-z" else 1 if mode == "sample-id" else 5 if mode == "sparse" else 15
    require(mask == expected_mask, "Incorrect component validity")
    rate = mode in ("sample", "noperspective sample", "trigger", "sample-id")
    require(report["sample_frequency"] == rate, "Incorrect sample-frequency declaration")
    require(report["adapter_vendor"] == (report["vendor_request"] or 5140), "Unexpected adapter selection")
    require(report["modules"], "Module evidence is missing")
    for module in report["modules"]:
        name = module.lower().replace("\\", "/")
        require("renderdoc" not in name and "/intelswtools/gpa/" not in name,
                "Forbidden analysis runtime was loaded")

    def read(name):
        raw = replacements.get(name, (directory / name).read_bytes())
        require(len(raw) == 256 * samples * 16, "Incorrect raw output length")
        return list(struct.iter_unpack("<4I", raw))

    reference = read("reference.bin")
    oracle = [read(f"oracle-{i}.bin") for i in range(4)]
    targets = [read(f"target-{i}.bin") for i in range(8)]
    direct = neighbors = 0
    for y in range(2, 14):
        for x in range(2, 14):
            for sample in range(samples):
                index = (y * 16 + x) * samples + sample
                px, py, rx, ry = targets[4][index]
                require((px, py) == (x, y) and rx in (0, 1) and ry in (0, 1), "Invalid pixel/role")
                lane = rx + 2 * ry
                direction = struct.unpack("<4f", struct.pack("<4I", *targets[7][index]))
                require(direction == (-1.0 if rx else 1.0, -1.0 if ry else 1.0, 0.0, 0.0),
                        "Invalid orientation derivatives")
                require(targets[6][index][lane] & (1 << sample), "Writing invocation lacks sample coverage")
                for c in range(4):
                    if mask & (1 << c):
                        require(targets[lane][index][c] == reference[index][c], "Direct input bits differ")
                        direct += 1
                for neighbor in range(4):
                    require(targets[5][index][neighbor] == (sample if rate else 0), "Sample identity differs")
                    for c in range(4):
                        if mask & (1 << c):
                            require(targets[neighbor][index][c] == oracle[neighbor][index][c],
                                    "Independent quad input bits differ")
                            neighbors += 1
    assembly = (directory / "helper-ps.asm.txt").read_text().lower()
    require("deriv_rtx_fine" in assembly and "deriv_rty_fine" in assembly,
            "Fine derivative transport is missing")
    for forbidden in ("dcl_uav", "dcl_resource", "dcl_constantbuffer", "dcl_sampler", "discard"):
        require(forbidden not in assembly, "Original resource or discard behavior leaked into helper")
    return {"case": directory.name, "direct_components": direct, "neighbor_components": neighbors,
            "adapter_vendor": report["adapter_vendor"], "adapter_device": report["adapter_device"],
            "adapter_luid": report["adapter_luid"], "driver_version": report["driver_version"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    cases = sorted(p.parent for p in args.directory.glob("*/report.json"))
    expected = {f"{vendor}-{mode.replace(' ', '-')}-{samples}"
                for vendor in (0, 4318, 32902) for mode in MODES for samples in (1, 4)}
    require({p.name for p in cases} == expected, "Incomplete fixture matrix")
    runs = [validate(case) for case in cases]
    controls = {}
    case = args.directory / "0-linear-1"
    for name in ("reference.bin", "oracle-0.bin", "target-0.bin", "target-4.bin", "target-5.bin", "target-7.bin"):
        raw = bytearray((case / name).read_bytes())
        offset = (2 * 16 + 2) * 16
        raw[offset] ^= 1
        try:
            validate(case, {name: bytes(raw)})
        except ValueError as error:
            controls[name] = str(error)
        else:
            raise ValueError(f"Corruption was accepted: {name}")
    result = {"schema": 1, "cases": len(runs), "runs": runs,
              "direct_components": sum(r["direct_components"] for r in runs),
              "neighbor_components": sum(r["neighbor_components"] for r in runs),
              "negative_controls": controls,
              "artifacts": [identity(p) for case in cases for p in sorted(case.iterdir()) if p.is_file()]}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps({k: v for k, v in result.items() if k not in ("runs", "artifacts")}, indent=2))


if __name__ == "__main__":
    main()
