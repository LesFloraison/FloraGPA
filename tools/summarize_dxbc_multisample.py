"""Validate and retain native DXBC multisample comparisons and independent stencil probes."""

import argparse
import itertools
import json
from pathlib import Path
import re

from summarize_dxbc_execution import identity
from summarize_dxbc_texture_uav import read_run


def matrix(path):
    result = read_run(path)
    text = path.read_text(encoding="utf-8-sig")
    result["hardware"] = []
    result["independent_probes"] = []
    result["dispatch_rows"] = []
    result["resource_clamps"] = []
    result["native_comparisons"] = []
    for line in text.splitlines():
        if not line.startswith("QINFO") or " {" not in line:
            continue
        item = json.loads(line[line.index(" {") + 1:])
        if "records" in item:
            result["independent_probes"].append(item)
        else:
            case = re.search(r"DxbcMultisampleTests::([^ ]+)", line).group(1)
            item["test_passed"] = bool(re.search(
                r"^PASS\s+: DxbcMultisampleTests::" + re.escape(case) + r"$", text, re.MULTILINE))
            collection = "native_comparisons" if item.get("probe_native_comparison") else "dispatch_rows" if item.get("dispatch_rows") else "resource_clamps" if item.get("resource_clamp_test") else "hardware"
            result[collection].append(item)
    result["boundary_test_passed"] = bool(re.search(
        r"^PASS\s+: DxbcMultisampleTests::unavailableAndValidation\(\)$", text, re.MULTILINE))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("log", "regression", "build", "source", "probe"):
        parser.add_argument("--" + name, type=Path, action="append", default=[], required=name == "log")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    runs = [matrix(path) for path in args.log]
    final = runs[-1]
    expected = set(itertools.product([0, 4318, 32902], [2, 3, 4, 10, 28, 41, 42, 43, 139, 121, 122, 146, 147],
                                     [False, True]))
    cases = final["hardware"]
    complete = len(cases) == len(expected) and {
        (p["vendor"], p["format"], p["array"]) for p in cases} == expected
    independent = final["independent_probes"]
    expected_probes = {(v, enabled, lod) for v in [0, 4318, 32902]
                       for enabled, lod in [(False, 0), (True, 0), (True, .25), (True, 1)]}
    probe_ok = len(independent) == 12 and {(p["vendor"], p.get("resource_clamp", False), p.get("requested_min_lod", 0))
                                          for p in independent} == expected_probes
    alphas = {}
    adapters = {}
    for probe in independent:
        vendor = probe["vendor"]
        records = probe["records"]
        expected_records = {(fmt, array, (x, y, z, sample))
                            for fmt, array, sample in itertools.product([22, 47], [False, True], [0, 3])
                            for x, y, z in [(1, 2, 0), (4, 0, 0), (4294967295, 0, 0), (0, 4, 0), (0, 0, 2)]}
        actual_records = {(r["format"], r["array"], tuple(r["address"])) for r in records}
        probe_ok &= len(records) == 40 and actual_records == expected_records and probe["validation_errors"] == 0
        values = set()
        for record in records:
            x, y, z, _ = record["address"]
            outside = x >= 4 or y >= 4 or (record["array"] and z >= 2)
            bits = record["bits"]
            probe_ok &= len(bits) == 4 and bits[:3] == [0, 0 if outside or probe.get("requested_min_lod", 0) > 0 else 90, 0] and bits[3] in (0, 1)
            values.add(bits[3])
        probe_ok &= len(values) == 1 and (not vendor or probe["adapter_vendor"] == vendor)
        alphas[vendor] = sorted(values)
        adapters[vendor] = probe["adapter_luid"]
    regressions = [read_run(path) for path in args.regression]
    large = final["dispatch_rows"]
    clamps = final["resource_clamps"]
    comparisons = final["native_comparisons"]
    extra_ok = len(large) == 3 and {p["vendor"] for p in large} == {0, 4318, 32902} and all(
        p["values"] == 4194305 for p in large)
    extra_ok &= len(clamps) == 9 and {(p["vendor"], p["lod"]) for p in clamps} == set(
        itertools.product([0, 4318, 32902], [0, .25, 1]))
    extra_ok &= len(comparisons) == 6 and {(p["vendor"], p["format"]) for p in comparisons} == set(
        itertools.product([0, 4318, 32902], [139, 147]))
    for item in comparisons:
        comparison = item["native_comparison"]
        samples = item["multisample_inputs"]
        extra_ok &= (comparison["passed"] and comparison["matching_native_inputs"]
                     and comparison["original_observed_resources_unchanged"]
                     and comparison["multisample_inputs_checked"] == len(samples) == 1
                     and samples[0]["value_count"] == 129 and samples[0]["bytes"] == 2064
                     and item["steps"] > 0 and len(comparison["resources"]) == 1
                     and all(r["equal"] and r["expected_sha256"] == r["native_sha256"]
                             for r in comparison["resources"]))
    extra_ok &= all(p["passed"] and p["test_passed"] and p["adapter_luid"] == adapters.get(p["vendor"])
                    for p in [*large, *clamps, *comparisons])
    success = complete and probe_ok and extra_ok and final["boundary_test_passed"] and final["passed"] == 99 and all(
        p["passed"] and p["test_passed"] and p["source"] == "independent-replay-resource-inputs/5"
        and p["adapter_luid"] == adapters.get(p["vendor"]) for p in cases) and all(
            r["completed"] and r["failed"] == 0 and r["skipped"] == 0 for r in [final, *regressions])
    builds = []
    for path in args.build:
        text = path.read_text(encoding="utf-8-sig")
        builds.append({**identity(path), "compiler_errors": [line for line in text.splitlines()
            if re.search(r"error (?:C|LNK)\d+", line)]})
    for run in runs:
        run["independent_probe_summaries"] = [
            {**{key: value for key, value in probe.items() if key != "records"},
             "record_count": len(probe["records"]),
             "observed_bits": sorted({tuple(record["bits"]) for record in probe["records"]})}
            for probe in run.pop("independent_probes")]
        if run is not final:
            for key in ("hardware", "dispatch_rows", "resource_clamps", "native_comparisons"):
                run[key + "_count"] = len(run.pop(key))
    for regression in regressions:
        regression["probe_count"] = len(regression.pop("probes"))
    report = {"schema": 1, "scope": "native_dxbc_multisample_execution", "complete_release_acceptance": False,
              "sample_count_tested": 4, "hardware_matrix_complete": complete, "independent_probes_valid": probe_ok,
              "dispatch_and_clamp_validation_passed": extra_ok,
              "observed_stencil_alpha": alphas, "latest_validation_passed": bool(success),
              "runs": runs, "regressions": regressions, "builds": builds,
              "sources": [identity(path) for path in args.source],
              "standalone_probes": [{**identity(path), "result": json.loads(path.read_text())} for path in args.probe]}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(runs)} runs; complete multisample checkpoint validation: {bool(success)}")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
