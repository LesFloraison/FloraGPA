"""Validate independent sampling control-flow observations without accepting an executor."""

import argparse
import itertools
import json
import math
from pathlib import Path
import struct

from summarize_dxbc_execution import identity


def bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def source_index(flow, lane, slot, count):
    if flow == "early-return" and lane % 3 == 1:
        return None
    limit = 1 + lane % 3 if flow == "uneven-loop" else 3 if flow == "barrier-loop" else 1
    if slot >= limit:
        return None
    if flow == "branches":
        return lane if lane & 1 else (lane + 1) % count
    if flow == "staggered":
        return (lane + (3 if lane & 1 else 0)) % count
    if flow == "barrier-loop":
        return (lane + 1 + slot) % count
    return (lane + slot) % count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "binary", "out"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    shapes = ((64, 1, 1), (8, 4, 2), (4, 4, 4), (32, 2, 2))
    flows = ("uniform", "early-return", "branches", "uneven-loop", "barrier-loop", "staggered")
    expected_keys = set(itertools.product(shapes, flows, (0, 1), (0, 85)))
    shaders, reports = {}, []
    counts = {"cases": 0, "written_slots": 0, "untouched_slots": 0, "analytical_point_samples": 0}
    for vendor in (0, 4318, 32902):
        directory = args.root / str(vendor)
        path = directory / "results.json"
        report = json.loads(path.read_text(encoding="utf-8"))
        if (report["schema"] != 1 or report["scope"] != "independent_sample_group"
                or report["vendor"] != vendor or report["validation_errors"]
                or report["adapter_vendor"] != (vendor or 5140)):
            raise ValueError("Invalid independent adapter/report identity")
        texture = [bits(v) for mip in range(4) for y in range(8 >> mip)
                   for x in range(8 >> mip) for v in (mip + .25, x / 8, y / 8, 1)]
        if report["texture_bits"] != texture:
            raise ValueError("Unexpected independent texture contents")
        seen = set()
        for item in report["cases"]:
            shape, flow, offset, filtering = tuple(item["group_size"]), item["flow"], item["offset"], item["filter"]
            key = shape, flow, offset, filtering
            if key not in expected_keys or key in seen:
                raise ValueError("Unknown or duplicate flow observation")
            seen.add(key)
            count = math.prod(shape)
            if len(item["output"]) != count * 4 or len(item["parameters"]) != count * 3:
                raise ValueError("Incomplete input/output storage")
            gradients = ((0, 0, 0, 0), (.03125, 0, 0, .03125), (.125, 0, 0, .125),
                         (.25, 0, 0, .25), (1, 0, 0, 1), (-.25, 0, 0, -.25),
                         (1, 0, 0, .03125), (0, 1, -.03125, 0), (.25, .125, -.125, .25))
            coordinates = (-1.25, -.125, 0, .25, .5, 1, 1.25)
            parameters = []
            for lane in range(count):
                dx, dy, ex, ey = gradients[lane % 9]
                parameters += [[coordinates[(lane // 9) % 7], .375, 0, 0],
                               [dx, dy, 0, 0], [ex, ey, 0, 0]]
            if item["parameters"] != parameters:
                raise ValueError("Unexpected independent sampling inputs")
            if item["sampler_descriptor"] != [filtering, 3, 3, 3, 0, 16 if filtering else 0,
                                              1, 0, 0, 0, 0, 0, 0x7f7fffff]:
                raise ValueError("Unexpected observed sampler")
            name = item["shader"]
            if name != f"{flow}-{'-'.join(map(str, shape))}-{offset}.dxbc":
                raise ValueError("Unexpected shader identity")
            shader = identity(directory / name)
            if name in shaders and shaders[name] != shader["sha256"]:
                raise ValueError("Independent shader changed between adapters")
            shaders[name] = shader["sha256"]
            assembly = (directory / name.replace(".dxbc", ".asm.txt")).read_text(encoding="utf-8")
            required = ["sample_d", "dcl_thread_group " + ", ".join(map(str, shape))]
            if flow in ("branches", "staggered"):
                required += ["if_nz"]
            if flow in ("uneven-loop", "barrier-loop", "staggered"):
                required += ["loop", "endloop"]
            if flow == "barrier-loop":
                required += ["sync_g_t"]
            if any(token not in assembly for token in required):
                raise ValueError("Compiler removed a required control-flow operation")
            for lane in range(count):
                for slot in range(4):
                    index = source_index(flow, lane, slot, count)
                    output = item["output"][lane * 4 + slot]
                    if index is None:
                        if output != [0xdeadbeef] * 4:
                            raise ValueError("A nonparticipating invocation or slot was overwritten")
                        counts["untouched_slots"] += 1
                        continue
                    if len(output) != 4 or output[3] != bits(1) or 0xdeadbeef in output:
                        raise ValueError("A participating invocation did not write a complete texel")
                    counts["written_slots"] += 1
                    # Use exact powers of two only: no assumed anisotropic or skew LOD approximation.
                    if filtering == 0 and index % 9 < 8:
                        address, dx, dy = item["parameters"][index * 3:index * 3 + 3]
                        rho = 8 * max(math.hypot(*dx[:2]), math.hypot(*dy[:2]))
                        mip = min(3, max(0, int(math.floor(math.log2(rho) + .5)))) if rho else 0
                        width = 8 >> mip
                        x = min(width - 1, max(0, math.floor(address[0] * width) - offset))
                        y = min(width - 1, max(0, math.floor(address[1] * width) + offset))
                        expected = [bits(v) for v in (mip + .25, x / 8, y / 8, 1)]
                        if output != expected:
                            raise ValueError(f"Analytical point sample differs: {vendor} {key} {lane} {slot}")
                        counts["analytical_point_samples"] += 1
            counts["cases"] += 1
        if seen != expected_keys:
            raise ValueError("Missing control-flow observations")
        reports.append(identity(path))
    result = {"schema": 1, "scope": "independent_sample_group", "evidence_validated": True,
              "implementation_accepted": False, "complete_release_acceptance": False,
              **counts, "reports": reports, "shaders": shaders, "binary": identity(args.binary),
              "sources": [identity(Path(p)) for p in ("tools/native/sample_group_probe.cpp",
                                                     "tools/summarize_sample_groups.py")]}
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(counts))


if __name__ == "__main__":
    main()
