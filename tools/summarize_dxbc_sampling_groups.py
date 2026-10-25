"""Validate complete independent group comparisons and retain the initial failure evidence."""

import argparse
import itertools
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity
from summarize_dxbc_texture_uav import read_run


def observations(path):
    result = []
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if line.startswith("QINFO") and " {" in line:
            result.append(json.loads(line[line.index(" {") + 1:]))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("log", "initial", "reference", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("regression", "source", "build", "binary", "evidence"):
        parser.add_argument("--" + name, type=Path, action="append", default=[])
    parser.add_argument("--backend", default="dxbc-gpu/13")
    args = parser.parse_args()
    rows = observations(args.log)
    groups = [r for r in rows if r.get("sampling_group")]
    expected = set(itertools.product((0, 4318, 32902), range(96)))
    if len(groups) != 288 or {(r["vendor"], r["configuration"]) for r in groups} != expected:
        raise ValueError("Incomplete group comparison matrix")
    initial = read_run(args.initial)
    initial_rows = observations(args.initial)
    if (initial["failed"] != 2 or initial["skipped"] or
            {(r["vendor"], r["configuration"]) for r in initial_rows if r.get("differences")} != {(0, 9), (0, 21)}):
        raise ValueError("Initial divergent sampling failures were not preserved")
    reports = {v: json.loads((args.reference / str(v) / "results.json").read_text(encoding="utf-8"))
               for v in (0, 4318, 32902)}
    for row in groups:
        vendor = row["vendor"]
        item = reports[vendor]["cases"][row["configuration"]]
        if any(row[k] != item[k] for k in ("flow", "group_size", "filter", "offset")):
            raise ValueError("Executor comparison changed the independent configuration")
        batches = 2 if row["flow"] == "branches" else 3 if row["flow"] in ("uneven-loop", "barrier-loop") else 1
        luid = (row["helper"]["luid_high"] & 0xffffffff) << 32 | row["helper"]["luid_low"]
        if (row["differences"] or row["sampling_batches"] != batches or row["gpu_operations"] != batches
                or row["barriers"] != (6 if row["flow"] == "barrier-loop" else 0)
                or row["execution_model"] != "bounded-sampling-rendezvous/3"
                or row["helper"]["backend"] != args.backend or luid != reports[vendor]["adapter_luid"]
                or row["shader_sha256"] != identity(args.reference / str(vendor) / item["shader"])["sha256"]):
            raise ValueError("Invalid complete group comparison")
    progress = [r for r in rows if r.get("bounded_sampling_progress")]
    cancelled = [r for r in rows if r.get("sampling_wait_cancelled")]
    for subset in (progress, cancelled):
        if len(subset) != 3 or {r["vendor"] for r in subset} != {0, 4318, 32902}:
            raise ValueError("Incomplete progress/cancellation matrix")
    if any(not (4096 < r["steps"] < 5000) or r["sampling_batches"] != 1 for r in progress):
        raise ValueError("Unbounded or incorrect sampling progress")
    if any(r["checks"] != 1001 or not r["retry_finished"] for r in cancelled):
        raise ValueError("Cancellation/retry failed")
    final = read_run(args.log)
    regressions = [read_run(path) for path in args.regression]
    expected_regressions = {("DxbcSampleLevelTests", 262), ("DxbcGroupTests", 38),
                            ("DxbcExecutionTests", 375)}
    actual_regressions = set()
    for path, run in zip(args.regression, regressions):
        match = re.search(r"Start testing of (\w+)", path.read_text(encoding="utf-8-sig"))
        if match:
            actual_regressions.add((match.group(1), run["passed"]))
    if len(regressions) != 3 or actual_regressions != expected_regressions:
        raise ValueError("Required gradient, group and scalar execution regressions are missing")
    if final["passed"] != 296 or any(not r["completed"] or r["failed"] or r["skipped"]
                                      for r in (final, *regressions)):
        raise ValueError("Required sampling tests did not pass")
    for run in (initial, final, *regressions):
        run.pop("probes")
    result = {"schema": 1, "scope": "native_dxbc_sampling_groups", "validation_passed": True,
              "complete_release_acceptance": False, "initial": initial, "final": final,
              "comparisons": {"cases": len(groups), "raw_differences": 0,
                              "steps": sum(r["steps"] for r in groups),
                              "gpu_operations": sum(r["gpu_operations"] for r in groups),
                              "by_vendor": {str(v): sum(r["vendor"] == v for r in groups) for v in reports},
                              "by_flow": {f: sum(r["flow"] == f for r in groups)
                                          for f in sorted({r["flow"] for r in groups})}},
              "bounded_progress": progress, "cancelled": cancelled,
              "regressions": regressions,
              "references": [identity(args.reference / str(v) / "results.json") for v in reports],
              "sources": [identity(p) for p in args.source], "builds": [identity(p) for p in args.build],
              "binaries": [identity(p) for p in args.binary],
              "supporting_evidence": [identity(p) for p in args.evidence]}
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Validated 288 complete groups and six progress/cancellation cases")


if __name__ == "__main__":
    main()
