"""Validate independent pixel derivative observations without assuming coarse lane selection."""

import argparse
import itertools
import json
from pathlib import Path
import struct

from summarize_dxbc_execution import identity

OPERATIONS = dict(ddx="deriv_rtx", ddy="deriv_rty", ddx_coarse="deriv_rtx_coarse",
                  ddx_fine="deriv_rtx_fine", ddy_coarse="deriv_rty_coarse", ddy_fine="deriv_rty_fine")


def floating(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def summarize(root):
    reports, shaders, observed_pairs = [], {}, {}
    counts = dict(cases=0, compiler_rejections=0, inactive_vectors=0, finite_fine_components=0,
                  finite_coarse_components=0, special_active_vectors=0)
    ordinary = [[1, -1, .25, 0], [3, -4, .5, 2], [8, -9, 1, 8], [20, -16, 2, 20]]
    ordinary = [[struct.unpack("<I", struct.pack("<f", v))[0] for v in row] for row in ordinary]
    special = [[0x80000000, 1, 0x7f800000, 0x7fc12345], [0, 0x007fffff, 0x7f800000, 0x3f800000],
               [0x80000000, 0x00800000, 0xff800000, 0x40000000], [0, 0x80800000, 0xff800000, 0x40400000]]
    for vendor in (0, 4318, 32902):
        path = root / str(vendor) / "results.json"
        report = json.loads(path.read_text(encoding="utf-8"))
        if (report["schema"] != 1 or report["scope"] != "independent_pixel_derivatives"
                or report["vendor"] != vendor or report["adapter_vendor"] != (vendor or 5140)
                or report["validation_errors"] != 0 or not report["adapter_luid"]):
            raise ValueError("Invalid derivative observation identity")
        reports.append(identity(path))
        rejections = report["compiler_rejections"]
        if (len(rejections) != 6 or {r["operation"] for r in rejections} != set(OPERATIONS)
                or any(r["flow"] != "divergent" or "X4014" not in r["diagnostic"] for r in rejections)):
            raise ValueError("Missing specific divergent gradient compilation rejection")
        counts["compiler_rejections"] += len(rejections)
        expected = {(op, flow, dataset, coverage, discard)
                    for op, flow, dataset, coverage in itertools.product(
                        OPERATIONS, ("uniform", "discard", "reconverge", "loop"), (0, 1), (15, 3, 5, 1))
                    for discard in ((1, 2, 5, 15) if flow == "discard" else (0,))}
        seen = set()
        for item in report["cases"]:
            op, flow, dataset, coverage, discarded = key = tuple(item[k] for k in
                ("operation", "flow", "dataset", "coverage", "discarded"))
            if key not in expected or key in seen:
                raise ValueError("Unexpected or duplicate derivative case")
            seen.add(key)
            profile = "ps_4_0" if op in ("ddx", "ddy") else "ps_5_0"
            name = f"{op}-{flow}"
            if (item["profile"] != profile or item["shader"] != name + ".dxbc"
                    or item["derivative_defined"] is not True or len(item["output"]) != 4
                    or item["inputs"] != (special if dataset else ordinary)):
                raise ValueError("Derivative inputs or shader identity changed")
            shader = path.parent / item["shader"]
            digest = identity(shader)["sha256"]
            if name in shaders and shaders[name] != digest:
                raise ValueError("Derivative shader changed between adapters")
            if name not in shaders:
                assembly = shader.with_suffix(".asm.txt").read_text(encoding="utf-8")
                if OPERATIONS[op] + " " not in assembly or profile not in assembly:
                    raise ValueError("Compiler removed or changed the derivative opcode")
                if flow == "discard" and "discard" not in assembly:
                    raise ValueError("Compiler removed discard")
                if flow == "loop" and "endloop" not in assembly:
                    raise ValueError("Compiler removed loop")
                shaders[name] = digest
            values = [[floating(b) for b in row] for row in item["inputs"]]
            if not dataset:
                for lane in range(4):
                    if flow in ("discard", "loop"):
                        values[lane] = [v + lane * (component + 1) for component, v in enumerate(values[lane])]
                    elif flow == "reconverge":
                        values[lane] = [v * 2 if lane & 1 else v - 3 for v in values[lane]]
            for lane, output in enumerate(item["output"]):
                if len(output) != 4 or any(type(b) is not int or b < 0 or b > 0xffffffff for b in output):
                    raise ValueError("Malformed derivative bits")
                if not (coverage & ~discarded) & (1 << lane):
                    if output != [0xdeadbeef] * 4:
                        raise ValueError("Uncovered or discarded pixel wrote a target")
                    counts["inactive_vectors"] += 1
                    continue
                if dataset:
                    counts["special_active_vectors"] += 1
                    continue
                pairs = ((0, 1), (2, 3)) if op.startswith("ddx") else ((0, 2), (1, 3))
                if op.endswith("fine"):
                    a, b = pairs[lane // 2 if op.startswith("ddx") else lane % 2]
                    if list(map(floating, output)) != [values[b][c] - values[a][c] for c in range(4)]:
                        raise ValueError("Fine derivative disagrees with independent finite arithmetic")
                    counts["finite_fine_components"] += 4
                else:
                    matches = [index for index, (a, b) in enumerate(pairs)
                               if list(map(floating, output)) == [values[b][c] - values[a][c] for c in range(4)]]
                    if not matches:
                        raise ValueError("Coarse observation does not match either finite adjacent pair")
                    observed_pairs.setdefault(f"{vendor}-{op}", set()).update(matches)
                    counts["finite_coarse_components"] += 4
            counts["cases"] += 1
        if seen != expected:
            raise ValueError("Incomplete derivative observation matrix")
    return dict(schema=1, reports=reports, shader_sha256=shaders, counts=counts,
                observed_coarse_pairs={k: sorted(v) for k, v in observed_pairs.items()},
                coarse_pair_is_portable_guarantee=False, captured_quad_alignment_validated=False,
                helper_uav_side_effects_validated=False, complete_release_acceptance=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    result = summarize(args.root)
    result["probe_binary"] = identity(args.binary)
    result["probe_source"] = identity(Path("tools/native/pixel_derivative_probe.cpp"))
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result["counts"]))


if __name__ == "__main__":
    main()
