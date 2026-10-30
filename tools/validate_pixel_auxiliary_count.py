"""Validate auxiliary-target counts without substituting diagnostic coverage for original coverage."""
import argparse
import copy
import itertools
import json
from pathlib import Path
import re
from validate_pixel_return_count import read, require, sha, verify


def check(data, vendor, kind, samples, targets, mode, alpha, selected, frame_hash):
    require([data[k] for k in ("vendor", "kind", "samples", "targets", "mode", "alpha_bits", "selected")]
            == [vendor, kind, samples, targets, mode, alpha, selected], "Wrong fixture selection")
    require("error" not in data and len(data["reports"]) == samples
            and len(data["reference"]) == len(data["reference_adapters"]) == 2, "Incomplete observations")
    auxiliary = selected == 0 and mode == 2
    require(data["private_rt0"] == (data["reference"][0] if auxiliary else []), "Original RT0 storage changed")
    require(data["private_rt0_formats"] == ([3 if kind.endswith("uint") else 4] * (2 * samples)
                                          if auxiliary else []), "Original RT0 format changed")
    component = 3 if auxiliary and targets == 1 else (1 if kind.endswith("uint") else 2)
    for phase in data["reference"]:
        require(len(phase) == samples, "Missing original sample")
        for value in phase:
            require(len(value) == 4 and all(type(x) is int and 0 <= x <= 0xffffffff for x in value)
                    and (value == [0] * 4 or value[:3] == [7, 11, 13]), "Invalid original storage")
    for sample, report in enumerate(data["reports"]):
        for adapter in data["reference_adapters"]:
            require(all(adapter[k] == report["adapter"][k] for k in
                        ("vendor", "device", "luid_low", "luid_high")), "Original adapter changed")
        expected = 130 * sum(phase[sample][0] == 7 for phase in data["reference"])
        if mode != 2:
            require(expected == (260 if mode == 0 else 130), "Reference violates owned geometry")
        verify(report, vendor, expected, component_type=component)
        require(report["frame_sha256"] == frame_hash, "Wrong source frame")
        transport = report["count_transport"]
        require([transport[k] for k in ("source_slot", "slot", "sample", "samples", "x", "y")]
                == [selected, 1 if auxiliary else selected, sample, samples, 1, 1]
                and transport["auxiliary_target"] is auxiliary, "Wrong transport routing")
        shader = report["completion_shader"]
        require(shader["coverage_output_declared"] is (mode == 3), "Wrong original coverage declaration")
        if auxiliary:
            require(shader["auxiliary_target"] is True and shader["original_rt0_instructions_preserved"] is True
                    and shader["output_declaration_added"] is (targets == 1), "Wrong output preservation")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--prior", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    groups = observations = auxiliary_observations = rejected = 0
    for vendor, kind, samples, targets, mode in itertools.product(
            (0, 4318, 32902), ("uint", "int", "min16uint", "min16int"), (1, 4), (1, 8), range(4)):
        alphas = (0, 1, 0x3f000000, 0x3f800000, 0xbf800000, 0x7fc00000) if mode == 2 else (
            (1, 0x3f800000) if mode == 0 else (0x3f800000,))
        for alpha, selected in itertools.product(alphas, (0,) if targets == 1 else (7, 0)):
            base = f"{vendor}-{kind}-{samples}-{targets}-{mode}-{alpha}"
            tag = base + ("-rt0" if targets == 8 and selected == 0 else "")
            path = args.root / "native" / (tag + ".json")
            data = read(path)
            frame_hash = sha(path.with_suffix(".gpa_frame"))
            for suffix in (".gpa_frame", ".reference-0.gpa_frame", ".reference-1.gpa_frame"):
                require(sha(path.with_suffix(suffix)) == sha(args.prior / "native" / (base + suffix)),
                        "Owned fixture changed from the retained experiment")
            require(data["reference"] == read(args.prior / "native" / (base + ".json"))["reference"],
                    "Original reference values changed")
            params = (vendor, kind, samples, targets, mode, alpha, selected, frame_hash)
            check(data, *params)
            auxiliary = selected == 0 and mode == 2
            for mutation in range(6):
                wrong = copy.deepcopy(data)
                report = wrong["reports"][0]
                if mutation == 0:
                    report["return_count"]["count"] += 1
                elif mutation == 1:
                    report["count_transport"]["source_slot"] += 1
                elif mutation == 2:
                    report["count_transport"]["auxiliary_target"] = not auxiliary
                elif mutation == 3:
                    report["completion_shader"]["coverage_output_declared"] = mode != 3
                elif mutation == 4:
                    wrong["reference_adapters"][0]["device"] += 1
                elif auxiliary:
                    wrong["private_rt0_formats"][0] = 2
                else:
                    report["completion_shader"]["alpha_storage_is_original_bits"] = True
                try:
                    check(wrong, *params)
                except ValueError:
                    rejected += 1
                else:
                    raise ValueError("Corrupted observation was accepted")
            groups += 1
            observations += samples
            auxiliary_observations += samples if auxiliary else 0
    controls = []
    for vendor, slot, counter in itertools.product((0, 4318, 32902), (1, 2), (False, True)):
        path = args.root / "native" / f"uav-{vendor}-{slot}-{int(counter)}.json"
        data = read(path)
        require([data[k] for k in ("vendor", "slot", "counter", "rejected")] == [vendor, slot, counter, slot == 1]
                and data["initial_counter"] == data["restored_counter"] == 0 and data["expected_counter"] == 1
                and data["restored"] is True and data["retry_matched"] is True, "Invalid UAV recovery")
        if slot == 2:
            verify(data["report"], vendor, 1 if vendor == 4318 else 0, component_type=3)
            require(data["report"]["frame_sha256"] == sha(path.with_suffix(".gpa_frame"))
                    and data["report"]["count_transport"]["slot"] == 1, "Wrong UAV fixture")
        else:
            require(data["report"] is None, "Rejected request returned a fabricated count")
        controls.append({k: data[k] for k in ("vendor", "slot", "counter", "rejected", "expected_counter")})
    require(groups == 720 and len(list((args.root / "native").glob("*.json"))) == 732, "Incomplete matrix")
    require(read(args.root / "exit.json")["exit_code"] == 0, "GPU process failed")
    match = re.search(r"Totals: (\d+) passed, (\d+) failed, (\d+) skipped, (\d+) blacklisted",
                      (args.root / "tests.txt").read_text(encoding="utf-8"))
    require(match is not None and tuple(map(int, match.groups())) == (734, 0, 0, 0), "GPU suite incomplete")
    summary = {"schema": 1, "scope": "native_auxiliary_pixel_return_counts", "configurations": groups,
               "observations": observations, "auxiliary_observations": auxiliary_observations,
               "corruptions_rejected": rejected, "uav_controls": controls, "qt_passed": 734,
               "original_fragment_eligibility": None}
    args.out.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
