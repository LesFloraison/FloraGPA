"""Retain independent batch/single sampling differences without relaxing assertions."""

import argparse
import itertools
import json
from pathlib import Path
import struct

from summarize_dxbc_execution import identity


def inputs(record):
    return tuple(record["parameters"] + record["ddx"] + record["ddy"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("batch", "single", "execution_log", "binary", "out"):
        parser.add_argument("--" + name.replace("_", "-"), type=Path, required=True)
    args = parser.parse_args()
    reports, identities, differences, expected_mismatches = {}, [], [], {}
    stage_ids = {"ps": 0, "vs": 1, "cs": 5}
    for stage, vendor in itertools.product(stage_ids, [0, 4318, 32902]):
        pair = []
        for mode, root in (("batch", args.batch), ("single", args.single)):
            path = root / stage / str(vendor) / "results.json"
            report = json.loads(path.read_text(encoding="utf-8-sig"))
            if (report["scope"] != "independent_sample_grad" or report["stage"] != stage
                    or report.get("invocation_mode", "batch") != mode or report["vendor"] != vendor
                    or report["validation_errors"] or len(report["cases"]) != 34):
                raise ValueError("Invalid invocation observation identity")
            identities.append(identity(path))
            pair.append(report)
        batch, single = pair
        if batch["adapter_luid"] != single["adapter_luid"]:
            raise ValueError("Invocation comparison changed adapter")
        reports[stage, vendor] = pair
        for configuration, (a, b) in enumerate(zip(batch["cases"], single["cases"])):
            if (a["status"] != "observed" or b["status"] != "observed"
                    or len(a["records"]) != 63 or len(b["records"]) != 63
                    or {k: v for k, v in a.items() if k != "records"}
                    != {k: v for k, v in b.items() if k != "records"}):
                raise ValueError("Invocation comparison changed sampling state")
            changed = []
            for index, (x, y) in enumerate(zip(a["records"], b["records"])):
                if inputs(x) != inputs(y):
                    raise ValueError("Invocation comparison changed inputs")
                if x["bits"] != y["bits"]:
                    delta = [abs(struct.unpack("<f", struct.pack("<I", p))[0]
                                 - struct.unpack("<f", struct.pack("<I", q))[0])
                             for p, q in zip(x["bits"], y["bits"])]
                    changed.append({"record": index, **{k: v for k, v in x.items() if k != "bits"},
                                    "batch_bits": x["bits"], "single_bits": y["bits"],
                                    "absolute_differences": delta})
                    expected_mismatches[vendor, configuration, stage_ids[stage], inputs(x)] = y["bits"]
            if changed:
                differences.append({"stage": stage, "vendor": vendor, "configuration": configuration,
                                    "name": a["name"], "offset": a["offset"], "records": changed})
    observed, groups = {}, {}
    for line in args.execution_log.read_text(encoding="utf-8-sig").splitlines():
        if not line.startswith("QINFO") or " {" not in line:
            continue
        row = json.loads(line[line.index(" {") + 1:])
        if row.get("sampling_mismatch"):
            key = row["vendor"], row["configuration"], row["stage"], inputs(row["input"])
            if row["valid"] != 15 or key in observed:
                raise ValueError("Invalid or duplicate executor mismatch")
            observed[key] = list(reversed(row["actual"]))
        if row.get("group_sampling_mismatch"):
            vendor, configuration = row["vendor"], row["configuration"]
            batch, single = reports["cs", vendor]
            actual = [list(r["bits"]) for r in batch["cases"][configuration]["records"]]
            for change in row["differences"]:
                offset = change["byte_offset"]
                i, c = offset // 16, offset // 4 % 4
                if offset % 4 or actual[i][c] != change["expected"]:
                    raise ValueError("Invalid group mismatch offset or expected value")
                actual[i][c] = change["actual"]
            if actual != [r["bits"] for r in single["cases"][configuration]["records"]]:
                raise ValueError("Executor group differs from independent single invocations")
            groups[vendor, configuration] = True
    if observed != expected_mismatches or set(groups) != {(0, 8), (0, 9)}:
        raise ValueError("Executor mismatch set does not match independent invocation differences")
    result = {"schema": 1, "scope": "independent_sample_grad_invocation_differences",
              "evidence_validated": True, "implementation_accepted": False,
              "complete_release_acceptance": False, "observations_per_mode": 19278,
              "mismatching_lane_records": len(observed), "mismatching_group_cases": len(groups),
              "executor_matches_single_invocations": True,
              "executor_matches_batch_invocations": False,
              "differences": differences, "reports": identities,
              "execution_log": identity(args.execution_log), "binary": identity(args.binary),
              "sources": [identity(Path(p)) for p in ("tools/native/sample_level_probe.cpp",
                                                     "tools/summarize_sample_grad_invocations.py")]}
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Validated invocation differences: {len(observed)} lane records, {len(groups)} group cases")


if __name__ == "__main__":
    main()
