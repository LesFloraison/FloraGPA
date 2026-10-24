"""Validate independent sample-info, sample-position and MS-view load observations."""

import argparse
import itertools
import json
from pathlib import Path
import struct

from summarize_dxbc_execution import identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    runs, shader_hashes = [], {}
    expected = set(itertools.product([1, 2, 4, 8, 16], [0, 4294967295, 4294967294], range(3), [False, True]))
    total, discrepancies = 0, []
    for vendor, multisample in itertools.product([0, 4318, 32902], [False, True]):
        directory = args.root / f"{vendor}-{int(multisample)}"
        path = directory / "results.json"
        report = json.loads(path.read_text())
        if (report["schema"] != 1 or report["scope"] != "independent_sample_queries"
                or report["vendor"] != vendor or report["multisample_enable"] != multisample
                or report["validation_errors"] != 0
                or vendor and report["adapter_vendor"] != vendor):
            raise ValueError("Unexpected probe identity or validation failure")
        cases = report["cases"]
        if len(cases) != 90 or {(c["count"], c["quality"], c["mode"], c["unbound"]) for c in cases} != expected:
            raise ValueError("Incomplete sample-query matrix")
        summaries = []
        for case in cases:
            rows = case["records"]
            count, quality, mode, unbound = (case[k] for k in ["count", "quality", "mode", "unbound"])
            if case["status"] == "unsupported_sample_count":
                if case["quality_levels"] != 0 or rows:
                    raise ValueError("Invalid unsupported-count evidence")
                summaries.append(case)
                continue
            if case["status"] != "observed" or not case["quality_levels"]:
                raise ValueError("Unresolved resource creation failure")
            if len(rows) != count + 2 or {r["sample"] for r in rows} != {*range(count + 1), 4294967295}:
                raise ValueError("Missing sample indices")
            samples = 1 if mode == 2 and unbound else 0 if unbound else count
            for row in rows:
                bits, sample = row["bits"], row["sample"]
                if len(bits) != 4 or bits[0] != samples:
                    raise ValueError("Sample count mismatch")
                load = 0x3e800000 if mode != 2 and not unbound and sample < count else 0
                if bits[3] != load:
                    raise ValueError("Native MS-view point-load mismatch")
                if (sample >= samples or samples <= 1) and bits[1:3] != [0, 0]:
                    raise ValueError("Outside or single-sample position mismatch")
                if quality == 4294967294 and bits[1:3] != [0, 0]:
                    discrepancies.append({"vendor": vendor, "multisample_enable": multisample,
                                          "count": count, "mode": mode, "unbound": unbound,
                                          "sample": sample, "position_bits": bits[1:3]})
                for value in bits[1:3]:
                    position = struct.unpack("<f", struct.pack("<I", value))[0]
                    if not -.5 <= position <= .5 or position * 16 != int(position * 16):
                        raise ValueError("Invalid sample position")
            total += len(rows)
            summaries.append({**{k: v for k, v in case.items() if k != "records"},
                              "record_count": len(rows), "sample_info": samples,
                              "positions": [r["bits"][1:3] for r in rows[:count]],
                              "load_bits": sorted({r["bits"][3] for r in rows})})
        identities = []
        for name in ["vertex.dxbc", *[f"query-{i}.dxbc" for i in range(3)]]:
            shader = directory / name
            binary = shader.read_bytes()
            if binary[:4] != b"DXBC" or struct.unpack_from("<I", binary, 24)[0] != len(binary):
                raise ValueError("Invalid DXBC shader")
            digest = identity(shader)
            if name in shader_hashes and shader_hashes[name] != digest["sha256"]:
                raise ValueError("Shader changed across adapters")
            shader_hashes[name] = digest["sha256"]
            identities.append(digest)
        for mode in range(3):
            assembly = directory / f"query-{mode}.asm.txt"
            text = assembly.read_text()
            if "sampleinfo_uint" not in text or "samplepos" not in text or (mode != 2 and "ldms" not in text):
                raise ValueError("Expected query/load instructions are missing")
            identities.append(identity(assembly))
        patterns = {}
        for case in summaries:
            if case["status"] != "observed" or case["unbound"]:
                continue
            key = (case["count"], case["quality"], tuple(map(tuple, case["positions"])))
            patterns.setdefault(key, []).append(case["mode"])
        runs.append({**identity(path), **{k: v for k, v in report.items() if k != "cases"},
                     "configurations": len(cases),
                     "observed_configurations": sum(c["status"] == "observed" for c in cases),
                     "unsupported_sample_counts": sorted({c["count"] for c in cases
                                                            if c["status"] == "unsupported_sample_count"}),
                     "records": sum(len(c["records"]) for c in cases),
                     "bound_position_patterns": [{"count": key[0], "quality": key[1], "positions": key[2],
                                                   "modes": modes} for key, modes in patterns.items()],
                     "shaders": identities})
    result = {"schema": 1, "scope": "independent_sample_query_research", "validation_passed": True,
              "center_pattern_queries_agree": not discrepancies,
              "center_pattern_discrepancies": discrepancies,
              "complete_release_acceptance": False, "observations": total, "runs": runs,
              "binary": identity(args.binary), "sources": [identity(Path(p)) for p in [
                  "tools/native/sample_query_probe.cpp", "tools/summarize_sample_queries.py"]]}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(f"Validated 540 configurations and {total} shader observations; "
          f"{len(discrepancies)} unresolved center-pattern query discrepancies")


if __name__ == "__main__":
    main()
