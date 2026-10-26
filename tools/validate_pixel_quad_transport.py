"""Independently validate saved D3D11 quad-transport bytes; never modify observations."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

MASK = 0xffffffff
CLEAR = (16777216,) * 4
MULTIPLIERS = (0x9e3779b9, 0x85ebca6b, 0xc2b2ae35, 0x27d4eb2f)
SPECIAL = (0, 0x80000000, 1, 0x007fffff, 0x00800000, 0x7f800000, 0xff800000, 0x7fc12345,
           0x7f812345, 0xffffffff, 0xdeadbeef, 0x01234567, 0x89abcdef, 0xaaaaaaaa, 0x55555555, 0x3f800000)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def identity(path):
    path = Path(path)
    data = path.read_bytes()
    return {"path": str(path), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def inputs(dataset):
    result, state = [], 0x13579bdf
    for pixel in range(256):
        row = []
        for component in range(4):
            if dataset:
                state ^= (state << 13) & MASK
                state ^= state >> 17
                state ^= (state << 5) & MASK
                row.append(state)
            else:
                row.append(SPECIAL[(pixel * 7 + component * 3) % 16])
        result.append(tuple(row))
    return result


def validate_case(directory, case, replacements=None):
    replacements = replacements or {}
    name, samples, rate = case["name"], case["samples"], case["sample_rate"]
    require(type(samples) is int and samples in (1, 2, 4, 8) and type(rate) is bool, "Invalid sample mode")
    read = lambda suffix: replacements.get(suffix, (directory / (name + suffix)).read_bytes())
    constants = read("-constants.bin")
    require(len(constants) == 4144, "Invalid constant buffer length")
    expected_inputs = inputs(case["dataset"])
    require(list(struct.iter_unpack("<4I", constants[:4096])) == expected_inputs, "Input bit fixture changed")
    targets = []
    for target in range(9):
        raw = read(f"-target-{target}.bin")
        require(len(raw) == 256 * samples * 16, "Invalid target storage length")
        targets.append(list(struct.iter_unpack("<4I", raw)))
    written = components = nonwriting = 0
    origins, roles = set(), set()
    for address, location in enumerate(targets[4]):
        captured = location[0] < 16 and location[1] < 16
        referenced = targets[8][address] != CLEAR
        require(captured == referenced, "Capture/reference coverage differs")
        if not captured:
            continue
        written += 1
        pixel, sample = divmod(address, samples)
        y, x = divmod(pixel, 16)
        rx, ry = location[2:]
        require(location[:2] == (x, y) and rx in (0, 1) and ry in (0, 1) and x >= rx and y >= ry,
                "Invalid measured position or quad role")
        ox, oy, lane = x - rx, y - ry, rx + 2 * ry
        origins.add((ox, oy))
        roles.add(lane)
        direction = struct.unpack("<4f", struct.pack("<4I", *targets[7][address]))
        require(direction == (-1.0 if rx else 1.0, -1.0 if ry else 1.0, 0.0, 0.0),
                "Orientation derivative is not a unit direction or numeric zero")
        require(targets[lane][address] == targets[8][address], "Current lane differs from direct shader output")
        require(targets[5][address][lane] == (sample if rate else 0), "Current sample identity differs")
        require(targets[6][address][lane] & (1 << sample) and case["sample_mask"] & (1 << sample),
                "Current coverage/sample mask does not permit the observed write")
        for neighbor in range(4):
            nx, ny = ox + (neighbor & 1), oy + (neighbor >> 1)
            sid = targets[5][address][neighbor]
            require(nx < 16 and ny < 16 and sid < (samples if rate else 1), "Neighbor outside fixture domain")
            expected = tuple(value ^ ((sid * multiplier) & MASK)
                             for value, multiplier in zip(expected_inputs[ny * 16 + nx], MULTIPLIERS))
            require(expected != CLEAR, "Fixture value collides with clear sentinel")
            require(targets[neighbor][address] == expected, "Neighbor raw bits differ")
            components += 4
            if targets[4][(ny * 16 + nx) * samples + sample][0] >= 16:
                nonwriting += 1
    require(written > 0 and case["passed"] is True and case["mismatches"] == [], "Empty or failed native case")
    require((written, components, nonwriting) ==
            (case["written_samples"], case["neighbor_components"], case["nonwriting_neighbors"]),
            "Native counts disagree with raw storage")
    require(sorted(origins) == [tuple(p) for p in case["origins"]], "Quad origins disagree")
    return {"name": name, "written_samples": written, "neighbor_components": components,
            "nonwriting_neighbors": nonwriting, "observed_lane_roles": sorted(roles),
            "observed_origins": sorted(origins)}


def negative_controls(directory, case):
    name = case["name"]
    location = (directory / (name + "-target-4.bin")).read_bytes()
    address = next(i for i, row in enumerate(struct.iter_unpack("<4I", location)) if row[0] < 16)
    controls = {}
    for kind, suffix, offset, value in (
        ("neighbor_bits", "-target-0.bin", address * 16, None),
        ("quad_role", "-target-4.bin", address * 16 + 8, 2),
        ("orientation", "-target-7.bin", address * 16, 0),
        ("sample_identity", "-target-5.bin", address * 16, 0xffffffff),
        ("reference_output", "-target-8.bin", address * 16, None),
        ("input_fixture", "-constants.bin", 0, None),
    ):
        raw = bytearray((directory / (name + suffix)).read_bytes())
        old = struct.unpack_from("<I", raw, offset)[0]
        struct.pack_into("<I", raw, offset, old ^ 1 if value is None else value)
        try:
            validate_case(directory, case, {suffix: bytes(raw)})
        except ValueError as error:
            controls[kind] = str(error)
        else:
            raise ValueError("Corrupt evidence was accepted: " + kind)
    return controls


def validate(directory, source):
    report = json.loads((directory / "report.json").read_text())
    require(report["schema"] == 1 and report["scope"] == "lossless_quad_transport_research" and
            report["passed"] is True and report["complete_release_acceptance"] is False, "Invalid report")
    require((directory / "source.hlsl").read_bytes() == source.read_bytes(), "Shader source changed")
    require(report["loaded_modules"], "Missing runtime module audit")
    for module in report["loaded_modules"]:
        path = module.replace("\\", "/").lower()
        require("renderdoc" not in path and "/intelswtools/gpa/" not in path and
                path.rsplit("/", 1)[-1] not in {"dx11_player.dll", "dx11_playback.dll", "shimloader64.dll", "shimd3d64.dll"},
                "External analysis module loaded")
    for assembly in directory.glob("*-capture.dxbc.asm.txt"):
        text = assembly.read_text().lower()
        require("dcl_uav" not in text and "deriv_rtx_fine" in text and "deriv_rty_fine" in text,
                "Capture program lacks fine derivatives or requires a pixel UAV")
    cases = report["cases"]
    available = set(c["samples"] for c in cases)
    unsupported = report["unsupported_sample_counts"]
    require(1 in available and available.isdisjoint(unsupported) and
            available | set(unsupported) == {1, 2, 4, 8}, "Sample support accounting differs")
    expected = {(s, r, m, d) for s in available for r in (False, True) for m in range(8) for d in range(2)}
    actual = {(c["samples"], c["sample_rate"], c["mode"], c["dataset"]) for c in cases}
    require(actual == expected and len(cases) == len(expected), "Case matrix is incomplete or duplicated")
    rows = [validate_case(directory, case) for case in cases]
    files = [identity(path) for path in sorted(directory.iterdir()) if path.is_file()]
    return {"directory": str(directory), "passed": True, "cases": len(rows),
            "adapter_vendor": report["adapter_vendor"], "adapter_device": report["adapter_device"],
            "adapter_luid": report["adapter_luid"], "driver_version": report["driver_version"],
            "unsupported_sample_counts": unsupported,
            "written_samples": sum(r["written_samples"] for r in rows),
            "neighbor_components": sum(r["neighbor_components"] for r in rows),
            "nonwriting_neighbors": sum(r["nonwriting_neighbors"] for r in rows),
            "negative_controls": negative_controls(directory, cases[0]), "rows": rows, "artifacts": files}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+", type=Path)
    parser.add_argument("--source", type=Path, default=Path(__file__).parent / "native/pixel_quad_transport.hlsl")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    rows = [validate(directory, args.source) for directory in args.directories]
    result = {"schema": 1, "passed": True, "complete_release_acceptance": False, "runs": rows}
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "cases": sum(row["cases"] for row in rows),
                      "neighbor_components": sum(row["neighbor_components"] for row in rows),
                      "negative_controls": sum(len(row["negative_controls"]) for row in rows)}))


if __name__ == "__main__":
    main()
