"""Compare single-sample coverage with its bit model while retaining original GPU deviations."""
import argparse
import copy
import hashlib
import itertools
import json
import sys
from pathlib import Path

from validate_pixel_ordinals import expected_fragment, require
from validate_pixel_ordinal_replay import REAL, native, real, values


def check(row, original, normalized, key):
    vendor, mask, flow, early, alpha = key
    require([row[k] for k in ("vendor", "mask", "flow", "early", "alpha")] == list(key), "Wrong semantic identity")
    identity = row["normalization"]
    require(row["original_adapter"] == row["normalized_adapter"] and
            row["original_adapter"]["vendor"] == (vendor or 5140), "Wrong coverage adapter")
    require(identity["backend"] == "native-single-sample-coverage/1" and identity["changed"] and
            identity["required_samples"] == 1 and identity["coverage_declaration_retained"] and
            identity["extra_temps"] == 1 and identity["extra_uav_slots"] == 0 and
            identity["original_fragment_eligibility"] is None, "Invalid coverage provenance")
    require(hashlib.sha256(original).hexdigest() == identity["source_sha256"] and
            hashlib.sha256(normalized).hexdigest() == identity["shader_sha256"], "Coverage shader identity differs")
    require(not row["original_diagnostics"] and not row["normalized_diagnostics"], "GPU validation failed")
    require(row["original_counter"] == row["normalized_counter"] == 1 and
            row["original_buffer"] == row["normalized_buffer"] == [7] + [0] * 15,
            "Return-time coverage changed earlier UAV side effects")
    expected = [255, 0, 0, 0] if flow == 1 or mask & 1 else [0, 0, 255, 255]
    return row["original"] != expected, row["normalized"] != expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--semantics", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--replay", type=Path)
    args = parser.parse_args()
    replay_root = args.replay or args.root / "native-final"
    original_errors, normalized_errors = [], []
    keys = list(itertools.product((0, 4318, 32902), (0, 1, 2, 0x80000000, 0xffffffff), range(5), (False, True), (False, True)))
    controls = 0
    for key in keys:
        stem = args.semantics / "-".join(str(int(v)) for v in key)
        row = json.loads(stem.with_suffix(".json").read_text(encoding="utf-8"))
        original = stem.with_suffix(".original.dxbc").read_bytes()
        normalized = stem.with_suffix(".normalized.dxbc").read_bytes()
        a, b = check(row, original, normalized, key)
        if a:
            original_errors.append({"key": key, "actual": row["original"]})
        if b:
            normalized_errors.append({"key": key, "actual": row["normalized"]})
        for mutation in range(4):
            bad = copy.deepcopy(row)
            if mutation == 0:
                bad["normalized"][0] ^= 1
            elif mutation == 1:
                bad["normalized_counter"] = 0
            elif mutation == 2:
                bad["normalization"]["source_sha256"] = "0" * 64
            else:
                bad["normalization"]["required_samples"] = 4
            try:
                detected = check(bad, original, normalized, key)[1]
            except ValueError:
                detected = True
            require(detected, "Corrupted coverage evidence accepted")
            controls += 1
    probes = {}
    for name, stencil, transformed in (("d24s8", 45, False), ("d32s8", 20, False),
                                       ("normalized-final", 45, True)):
        failures = []
        for vendor, layout in itertools.product((0, 4318, 32902), range(3)):
            folder = args.root / name / f"{vendor}-{layout}-1-3-8"
            row = json.loads((folder / "results.json").read_text(encoding="utf-8"))
            require(row["stencil_format"] == stencil and row["samples"] == row["extent"] == 1 and
                    row["float_output"] and row["actual_vendor"] == (vendor or 5140) and not row["diagnostics"],
                    "Wrong ordinal format control")
            code = (folder / "ps.dxbc").read_bytes()
            if transformed:
                original = (folder / "source.ps.dxbc").read_bytes()
                require(original == (args.root / "d24s8" / folder.name / "ps.dxbc").read_bytes(),
                        "Original ordinal bytecode changed")
                require(hashlib.sha256(code).hexdigest() == row["coverage_normalization"]["shader_sha256"],
                        "Normalized ordinal bytecode differs")
            errors = []
            for observation in row["rows"]:
                fragment = expected_fragment(3, 0, observation["count"], observation["rank"])
                expected = [[0, 0, 0, 0] if fragment is None else values(layout, 0, fragment, True)]
                require(observation["expected"] == expected, "Ordinal model changed")
                if observation["outputs"] != expected:
                    errors.append({"count": observation["count"], "rank": observation["rank"],
                                   "actual": observation["outputs"], "expected": expected})
            if errors:
                failures.append({"vendor": vendor, "layout": layout, "rows": errors})
        probes[name] = {"groups": 9, "failures": failures}
    native_errors = []
    replay_keys = list(itertools.product((0, 4318, 32902), range(3), (1, 4), range(4), (1, 8)))
    for key in replay_keys:
        filename = "extraction-" + "-".join(map(str, key)) + ".json"
        row = json.loads((replay_root / filename).read_text(encoding="utf-8"))
        baseline = json.loads((args.baseline / filename).read_text(encoding="utf-8"))
        require(len(row["rows"]) == len(baseline["rows"]), "Baseline observation count changed")
        for current, previous in zip(row["rows"], baseline["rows"]):
            require(current["expected"] == previous["expected"] and
                    current["report"]["frame_sha256"] == previous["report"]["frame_sha256"] and
                    current["report"]["shaders"] == previous["report"]["shaders"], "Original GPA fixture changed")
        errors = native(row, key)
        if errors:
            native_errors.append({"key": key, "rows": errors})
        if key[2] == 1 and key[3] == 3:
            for mutation in range(4):
                bad = copy.deepcopy(row)
                coverage = bad["rows"][0]["report"]["coverage_normalization"]
                field, value = (("source_sha256", "0" * 64), ("required_samples", 4),
                                ("changed", False), ("original_fragment_eligibility", True))[mutation]
                coverage[field] = value
                try:
                    native(bad, key)
                except ValueError:
                    controls += 1
                else:
                    raise ValueError("Altered native coverage provenance accepted")
    for name, vendor in itertools.product(REAL, (0, 4318, 32902)):
        folder = args.root / "real-final" / f"{name}-{vendor}"
        row = json.loads((folder / "report.json").read_text(encoding="utf-8"))
        files = {p.name: p.read_bytes() for p in folder.iterdir() if p.suffix in (".bin", ".dxbc")}
        real(row, files, name, vendor)
    result = {"schema": 1, "scope": "single_sample_coverage_return_normalization",
              "complete_release_acceptance": False, "semantic_cases": len(keys), "corruption_controls": controls,
              "original_gpu_failures": original_errors, "normalized_gpu_failures": normalized_errors,
              "ordinal_probes": probes,
              "native_replay_groups": len(replay_keys), "native_replay_failures": native_errors,
              "unchanged_gpa_frame_inputs": len(replay_keys), "real_recovery_controls": 9,
              "normalized_contract_passed": not normalized_errors and not native_errors and not probes["normalized-final"]["failures"],
              "strict_differential_passed": not original_errors and not normalized_errors,
              "artifacts": [{"path": p.as_posix(), "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                            for folder in (args.semantics, replay_root, args.root / "real-final",
                                           *(args.root / name for name in probes))
                            for p in sorted(folder.rglob("*")) if p.is_file()]}
    with args.out.open("x", encoding="utf-8", newline="\n") as out:
        json.dump(result, out, indent=2)
        out.write("\n")
    print(json.dumps({k: v for k, v in result.items() if k not in
                     ("artifacts", "original_gpu_failures", "normalized_gpu_failures", "native_replay_failures", "ordinal_probes")}))
    return int(not result["strict_differential_passed"] or not result["normalized_contract_passed"])


if __name__ == "__main__":
    sys.exit(main())
