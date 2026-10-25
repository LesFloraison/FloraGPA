"""Retain order-sensitive no-target sample queries without normalizing their values."""

import argparse
import itertools
import json
from pathlib import Path

from summarize_dxbc_execution import identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    runs = []
    for vendor, multisample, sequence in itertools.product([0, 4318, 32902], [0, 1], [1, 2]):
        directory = args.root / f"{vendor}-{multisample}-{sequence}"
        path = directory / "results.json"
        report = json.loads(path.read_text())
        if (report["schema"] != 1 or report["scope"] != "independent_sample_queries"
                or report["vendor"] != vendor or report["multisample_enable"] != bool(multisample)
                or report["no_target_sequence"] != sequence or report["validation_errors"]
                or vendor and report["adapter_vendor"] != vendor or len(report["cases"]) != 1):
            raise ValueError("Invalid sequence identity or diagnostics")
        case = report["cases"][0]
        count = 4 if sequence == 2 else 1
        if (case["status"] != "observed" or case["count"] != count or case["quality"]
                or case["mode"] != 2 or not case["unbound"] or not case["quality_levels"]
                or len(case["records"]) != count + 2
                or {r["sample"] for r in case["records"]} != {*range(count + 1), 4294967295}):
            raise ValueError("Incomplete no-target sequence")
        for row in case["records"]:
            if (len(row["bits"]) != 4 or row["bits"][3]
                    or any(type(x) is not int or not 0 <= x <= 4294967295 for x in row["bits"])
                    or row["bits"][0] > 32):
                raise ValueError("Invalid raw sample-query record")
        runs.append({**identity(path), **report, "shaders": [identity(directory / name) for name in [
            "vertex.dxbc", "query-0.dxbc", "query-1.dxbc", "query-2.dxbc", "query-2.asm.txt"]]})
    differences = []
    for vendor, multisample in itertools.product([0, 4318, 32902], [False, True]):
        selected = [r for r in runs if r["vendor"] == vendor and r["multisample_enable"] == multisample]
        before, after = [r["cases"][0]["records"][0]["bits"] for r in selected]
        if before != after:
            differences.append({"vendor": vendor, "multisample_enable": multisample,
                                "fresh_bits": before, "after_target_bits": after})
    report = {"schema": 1, "scope": "no_target_sample_query_sequences", "evidence_validated": True,
              "sequence_invariant": not differences, "differences": differences,
              "complete_release_acceptance": False, "runs": runs,
              "binary": identity(args.binary), "sources": [identity(Path(p)) for p in [
                  "tools/native/sample_query_probe.cpp", "tools/summarize_sample_query_sequences.py"]]}
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Validated {len(runs)} sequences; {len(differences)} unresolved order-sensitive results")


if __name__ == "__main__":
    main()
