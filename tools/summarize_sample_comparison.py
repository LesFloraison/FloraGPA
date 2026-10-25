"""Validate independent comparison sampling observations and invocation differences."""

import argparse
import itertools
import json
import math
from pathlib import Path
import struct

from summarize_dxbc_execution import identity


def floating(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("batch", "single", "binary", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    reports, shaders, differences = [], {}, []
    counts = {"defined_records": 0, "undefined_records": 0, "analytical_point_records": 0}
    for stage, vendor in itertools.product(("cs", "ps", "vs"), (0, 4318, 32902)):
        pair = []
        for mode, root in (("batch", args.batch), ("single", args.single)):
            path = root / stage / str(vendor) / "results.json"
            report = json.loads(path.read_text(encoding="utf-8"))
            if (report["scope"] != "independent_sample_compare_zero" or report["vendor"] != vendor
                    or report["stage"] != stage or report["invocation_mode"] != mode
                    or report["validation_errors"] or len(report["cases"]) != 56
                    or report["adapter_vendor"] != (vendor or 5140)):
                raise ValueError("Invalid comparison observation identity")
            pair.append(report)
            reports.append(identity(path))
            for offset in (0, 1):
                shader_path = path.parent / f"sample-compare-{offset}.dxbc"
                shader = identity(shader_path)["sha256"]
                key = f"{stage}-{mode}-{offset}"
                if key in shaders and shaders[key] != shader:
                    raise ValueError("Comparison shader changed between adapters")
                shaders[key] = shader
                asm = shader_path.with_suffix(".asm.txt").read_text(encoding="utf-8")
                if "sample_c_lz" not in asm or "mode_comparison" not in asm:
                    raise ValueError("Compiler removed the comparison operation")
            for configuration, item in enumerate(report["cases"]):
                if len(item["records"]) != 63 or item["offset"] != configuration % 2:
                    raise ValueError("Incomplete comparison records")
                undefined = item["name"] == "null-sampler"
                if item["status"] != ("undefined_missing_comparison_sampler" if undefined else "observed"):
                    raise ValueError("Undefined comparison results were accepted")
                if undefined:
                    if not item["diagnostics"] or any(d["id"] != 390 for d in item["diagnostics"]):
                        raise ValueError("Missing specific comparison sampler diagnostic")
                elif item["diagnostics"]:
                    raise ValueError("Unexpected D3D11 validation errors")
                for index, record in enumerate(item["records"]):
                    parameter_bits = record["parameter_bits"]
                    u, v, reference, _ = map(floating, parameter_bits)
                    expected_u = (-1.25, -.125, 0, .25, .5, 1, 1.25)[index // 9]
                    expected_refs = (0xbf800000, 0, 0x3e000000, 0x3e800000, 0x3f000000,
                                     0x3f400000, 0x3f800000, 0x7f800000, 0x7fc12345)
                    if u != expected_u or v != .375 or parameter_bits[2] != expected_refs[index % 9]:
                        raise ValueError("Unexpected comparison input bits")
                    counts["undefined_records" if undefined else "defined_records"] += 1
                    if undefined:
                        continue
                    output = record["bits"]
                    if len(output) != 4 or len(set(output)) != 1 or not 0 <= floating(output[0]) <= 1:
                        raise ValueError("Invalid comparison result replication or range")
                    if not item["resource_bound"] and output != [0] * 4:
                        raise ValueError("Unbound comparison resource did not return zero")
                    if configuration < 16:
                        function = configuration // 2 + 1
                        if item["comparison"] != function or item["name"] != "point":
                            raise ValueError("Unexpected comparison function matrix")
                        x = min(7, max(0, math.floor(u * 8) - item["offset"]))
                        y = min(7, max(0, 3 + item["offset"]))
                        depth = ((x + 2*y) % 8) / 8
                        outcomes = (False, reference < depth, reference == depth, reference <= depth,
                                    reference > depth, reference != depth, reference >= depth, True)
                        expected = 0x3f800000 if outcomes[function - 1] else 0
                        if output != [expected] * 4:
                            raise ValueError(f"Analytical comparison differs: {stage} {vendor} {mode} {configuration} {index}")
                        counts["analytical_point_records"] += 1
        if pair[0]["adapter_luid"] != pair[1]["adapter_luid"]:
            raise ValueError("Invocation comparison changed adapters")
        for configuration, (batch, single) in enumerate(zip(pair[0]["cases"], pair[1]["cases"])):
            if batch["status"] != "observed":
                continue
            for index, (a, b) in enumerate(zip(batch["records"], single["records"])):
                if a != b:
                    differences.append({"stage": stage, "vendor": vendor, "configuration": configuration,
                                        "record": index, "batch": a, "single": b})
    result = {"schema": 1, "scope": "independent_comparison_sampling", "evidence_validated": True,
              "implementation_accepted": False, "complete_release_acceptance": False, **counts,
              "invocation_differences": differences, "reports": reports, "shaders": shaders,
              "binary": identity(args.binary), "sources": [identity(Path(p)) for p in
              ("tools/native/sample_compare_probe.cpp", "tools/summarize_sample_comparison.py")]}
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({**counts, "invocation_differences": len(differences)}))


if __name__ == "__main__":
    main()
