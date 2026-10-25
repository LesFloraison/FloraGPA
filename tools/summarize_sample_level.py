"""Validate independent SampleLevel evidence while retaining adapter differences."""

import argparse
import itertools
import json
import math
from pathlib import Path
import struct

from summarize_dxbc_execution import identity


def bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    names = ["point", "linear", "point-bias", "linear-bias", "anisotropic", "minimum", "maximum",
             "resource-minimum", "view-mips", "border", "wrap", "mirror", "mirror-once",
             "minimum-filter", "maximum-filter", "null-sampler", "null-resource"]
    expected = set(itertools.product(names, range(2)))
    parameters = set(itertools.product([-1.25, -.125, 0, .25, .5, 1, 1.25], [.375],
                                      [-2, 0, .25, .5, 1, 1.5, 2, 3, 5], [0]))
    runs, tables, shader_hashes = [], {}, {}
    for vendor in [0, 4318, 32902]:
        directory = args.root / str(vendor)
        path = directory / "results.json"
        report = json.loads(path.read_text())
        if (report["schema"] != 1 or report["scope"] != "independent_sample_level"
                or report["vendor"] != vendor or report["validation_errors"]
                or vendor and report["adapter_vendor"] != vendor):
            raise ValueError("Invalid sample-level probe identity or diagnostics")
        cases = report["cases"]
        if len(cases) != 34 or {(c["name"], c["offset"]) for c in cases} != expected:
            raise ValueError("Incomplete sample-level configuration matrix")
        tables[vendor] = {(c["name"], c["offset"]): c for c in cases}
        summaries = []
        for case in cases:
            records = case["records"]
            if case["status"] == "unsupported_min_max_filtering":
                if report["min_max_filtering"] or case["name"] not in names[13:15] or records:
                    raise ValueError("Invalid unsupported min/max evidence")
                summaries.append(case)
                continue
            if (case["status"] != "observed" or len(records) != 63
                    or {tuple(r["parameters"]) for r in records} != parameters):
                raise ValueError("Incomplete sample-level input grid")
            requested, observed = case["sampler_descriptor"], case["observed_sampler_descriptor"]
            if case["name"] == "null-sampler":
                if requested is not None or observed is not None:
                    raise ValueError("Null sampler has a descriptor")
            else:
                for descriptor in (requested, observed):
                    if (not isinstance(descriptor, list) or len(descriptor) != 13
                            or any(type(v) is not int or not 0 <= v <= 4294967295 for v in descriptor)):
                        raise ValueError("Invalid native sampler descriptor")
                # D3D11 canonicalizes unused fields. Retain both descriptors and
                # reject differences in fields that affect this probe's sampling.
                unused = ({5} if requested[0] & 0x7f != 0x55 else set())
                if 4 not in requested[1:4]:
                    unused.update(range(7, 11))
                if any(a != b and (i not in unused or b != 0)
                       for i, (a, b) in enumerate(zip(requested, observed))):
                    raise ValueError("An active sampler field changed")
            if case["observed_resource_min_lod"] != (case["resource_min_lod"] or None):
                raise ValueError("Resource MinLOD differs from the requested state")
            for row in records:
                values = row["bits"]
                if (len(values) != 4 or any(type(v) is not int or not 0 <= v <= 4294967295 for v in values)
                        or any(not 0 <= struct.unpack("<f", struct.pack("<I", v))[0] <= 4 for v in values)):
                    raise ValueError("Invalid or unwritten sample-level output")
                if case["name"] == "null-resource" and values != [0, 0, 0, 0]:
                    raise ValueError("Null-resource sampling is not zero")
                if case["name"] in ("point", "point-bias"):
                    u, v, lod, _ = row["parameters"]
                    mip = min(3, max(0, math.floor(lod + (2 if case["name"] == "point-bias" else 0) + .5)))
                    width = 8 >> mip
                    x = min(width - 1, max(0, math.floor(u * width) - case["offset"]))
                    y = min(width - 1, max(0, math.floor(v * width) + case["offset"]))
                    if values != [bits(mip + .25), bits(x / 8), bits(y / 8), bits(1)]:
                        raise ValueError("Analytical point-sampling mismatch")
            summaries.append({**{k: v for k, v in case.items() if k != "records"}, "records": len(records),
                              "mip_channel_at_u_quarter": [r["bits"][0] for r in records
                                                          if r["parameters"][0] == .25]})
        for offset in range(2):
            if tables[vendor]["linear", offset]["records"] != tables[vendor]["null-sampler", offset]["records"]:
                raise ValueError("Null sampler disagrees with native linear/clamp state")
        shaders = []
        for offset, suffix in itertools.product(range(2), ["dxbc", "asm.txt"]):
            shader = directory / f"sample-level-{offset}.{suffix}"
            record = identity(shader)
            if shader.name in shader_hashes and shader_hashes[shader.name] != record["sha256"]:
                raise ValueError("Probe shader changed across adapters")
            shader_hashes[shader.name] = record["sha256"]
            if suffix == "asm.txt" and "sample_l" not in shader.read_text():
                raise ValueError("Explicit-LOD instruction is missing")
            shaders.append(record)
        runs.append({**identity(path), **{k: v for k, v in report.items() if k != "cases"},
                     "cases": summaries, "shaders": shaders})
    differences = []
    for vendor in [4318, 32902]:
        for key in sorted(expected):
            a, b = tables[0][key], tables[vendor][key]
            if a["status"] != "observed" or b["status"] != "observed":
                continue
            changed = [{"parameters": x["parameters"], "warp_bits": x["bits"], "native_bits": y["bits"]}
                       for x, y in zip(a["records"], b["records"]) if x["bits"] != y["bits"]]
            if changed:
                differences.append({"vendor": vendor, "name": key[0], "offset": key[1],
                                    "different_records": len(changed), "examples": changed[:8]})
    result = {"schema": 1, "scope": "independent_sample_level_research", "evidence_validated": True,
              "complete_release_acceptance": False, "adapter_results_identical": not differences,
              "cross_adapter_differences": differences,
              "observations": sum(len(c["records"]) for table in tables.values() for c in table.values()),
              "runs": runs, "binary": identity(args.binary),
              "sources": [identity(Path(p)) for p in ["tools/native/sample_level_probe.cpp",
                                                     "tools/summarize_sample_level.py"]]}
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Validated {result['observations']} observations; {len(differences)} adapter/configuration differences")


if __name__ == "__main__":
    main()
