"""Validate integer return counts against independently replayed original coverage."""
import argparse
import copy
import itertools
import json
from pathlib import Path
import re
from validate_pixel_return_count import read, require, sha, verify


def check(data, vendor, kind, samples, targets, mode, alpha, frame_hash):
    require([data[k] for k in ("vendor", "kind", "samples", "targets", "mode", "alpha_bits")]
            == [vendor, kind, samples, targets, mode, alpha], "Wrong fixture selection")
    unsupported = targets == 1 and mode == 2
    require(data["unsupported_integer_rt0_coverage"] is unsupported
            and data["rejected_selections"] == (samples if unsupported else 0), "Wrong coverage boundary")
    require("error" not in data and len(data["reports"]) == (0 if unsupported else samples)
            and len(data["reference"]) == len(data["reference_adapters"]) == 2,
            "Incomplete original references or observations")
    require(all(len(phase) == samples for phase in data["reference"]), "Missing reference sample")
    for phase in range(2):
        adapter = data["reference_adapters"][phase]
        require(adapter["vendor"] == (vendor or 5140) and adapter["device"] > 0, "Wrong reference adapter")
        for value in data["reference"][phase]:
            require(len(value) == 4 and all(type(x) is int and 0 <= x <= 0xffffffff for x in value)
                    and (value == [0] * 4 or value[:3] == [7, 11, 13]), "Invalid reference output")
    for sample, report in enumerate(data["reports"]):
        for phase in range(2):
            value = data["reference"][phase][sample]
            require(len(value) == 4 and all(type(x) is int and 0 <= x <= 0xffffffff for x in value)
                    and (value == [0] * 4 or value[:3] == [7, 11, 13]), "Invalid original integer output")
            require(all(data["reference_adapters"][phase][k] == report["adapter"][k]
                        for k in ("vendor", "device", "luid_high", "luid_low")), "Reference GPU changed")
        expected = 130 * sum(phase[sample][0] == 7 for phase in data["reference"])
        if mode != 2:
            require(expected == (260 if mode == 0 else 130), "Original coverage violates owned geometry")
        verify(report, vendor, expected, component_type=1 if kind.endswith("uint") else 2)
        require(report["frame_sha256"] == frame_hash, "Wrong frame digest")
        transport = report["count_transport"]
        require([transport[k] for k in ("slot", "sample", "samples", "x", "y", "width", "height")]
                == [targets - 1, sample, samples, 1, 1, 4, 4], "Wrong selected target/sample")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--pre-guard", type=Path)
    args = parser.parse_args()
    groups = observations = rejected = 0
    hashes = {}
    populations = {}
    boundary_selections = 0
    differences = []
    for vendor, kind, samples, targets, mode in itertools.product(
            (0, 4318, 32902), ("uint", "int", "min16uint", "min16int"), (1, 4), (1, 8), range(4)):
        alphas = (0, 1, 0x3f000000, 0x3f800000, 0xbf800000, 0x7fc00000) if mode == 2 else (
            (1, 0x3f800000) if mode == 0 else (0x3f800000,))
        for alpha in alphas:
            tag = f"{vendor}-{kind}-{samples}-{targets}-{mode}-{alpha}"
            path = args.root / "native" / (tag + ".json")
            data = read(path)
            identity = []
            for suffix in (".gpa_frame", ".reference-0.gpa_frame", ".reference-1.gpa_frame"):
                identity.append(sha(path.with_suffix(suffix)))
            key = (kind, samples, targets, mode, alpha)
            require(hashes.setdefault(key, identity) == identity, "Fixture differs between adapters")
            params = (vendor, kind, samples, targets, mode, alpha, identity[0])
            check(data, *params)
            boundary_selections += data["rejected_selections"]
            if args.pre_guard:
                prior = read(args.pre_guard / "native" / (tag + ".json"))
                require(prior["reference"] == data["reference"], "Original reference changed after guard")
                require(sha(args.pre_guard / "native" / (tag + ".gpa_frame")) == identity[0],
                        "Original fixture changed after guard")
                for sample, report in enumerate(prior["reports"]):
                    verify(report, vendor, component_type=1 if kind.endswith("uint") else 2)
                    expected = 130 * sum(phase[sample][0] == 7 for phase in prior["reference"])
                    if report["return_count"]["count"] != expected:
                        require(targets == 1 and mode == 2, "Difference outside guarded boundary")
                        differences.append({"tag": tag, "sample": sample, "expected": expected,
                                            "diagnostic": report["return_count"]["count"]})
            for report in data["reports"]:
                count = report["return_count"]["count"]
                populations[count] = populations.get(count, 0) + 1
            for mutation in range(2 if data["unsupported_integer_rt0_coverage"] else 5):
                wrong = copy.deepcopy(data)
                if data["unsupported_integer_rt0_coverage"]:
                    if mutation == 0:
                        wrong["unsupported_integer_rt0_coverage"] = False
                    else:
                        wrong["rejected_selections"] -= 1
                    try:
                        check(wrong, *params)
                    except ValueError:
                        rejected += 1
                        continue
                    raise ValueError("Corrupted boundary was accepted")
                report = wrong["reports"][0]
                if mutation == 0:
                    report["return_count"]["count"] += 1
                elif mutation == 1:
                    report["completion_shader"]["marker_precision"] = "minimum"
                elif mutation == 2:
                    report["completion_shader"]["alpha_storage_is_original_bits"] = True
                elif mutation == 3:
                    wrong["reference_adapters"][0]["device"] += 1
                else:
                    report["completion_shader"]["component_type"] = 3
                try:
                    check(wrong, *params)
                except ValueError:
                    rejected += 1
                else:
                    raise ValueError("Corrupted evidence was accepted")
            groups += 1
            observations += len(data["reports"])
    require(len(list((args.root / "native").glob("*.json"))) == groups == 480, "Unexpected evidence set")
    require(read(args.root / "exit.json")["exit_code"] == 0, "GPU suite failed")
    match = re.search(r"Totals: (\d+) passed, (\d+) failed, (\d+) skipped, (\d+) blacklisted",
                      (args.root / "tests.txt").read_text(encoding="utf-8"))
    require(match is not None and tuple(map(int, match.groups())) == (482, 0, 0, 0), "Incomplete Qt run")
    summary = {"schema": 1, "scope": "private_integer_output_return_counts", "groups": groups,
               "observations": observations, "corruptions_rejected": rejected, "populations": populations,
               "rejected_boundary_selections": boundary_selections, "pre_guard_differences": differences,
               "original_fragment_eligibility": None, "qt_passed": 482}
    args.out.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in summary.items() if k != "pre_guard_differences"}, indent=2))


if __name__ == "__main__":
    main()
