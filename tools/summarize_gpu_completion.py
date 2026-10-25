"""Validate independent D3D11 completion observations without accepting release performance."""

import argparse
import itertools
import json
import math
from pathlib import Path
import statistics

from summarize_dxbc_execution import identity


def validate(report):
    vendor = report["vendor"]
    supported = report["fence_supported"]
    if (report["schema"] != 1 or report["scope"] != "independent_gpu_completion"
            or vendor not in (0, 4318, 32902) or report["adapter_vendor"] != (vendor or 5140)
            or not report["completed"] or report["unexpected_validation_errors"]
            or report["production_wait_changed"] or report["performance_acceptance"]):
        raise ValueError("Invalid completion probe identity or status")
    if supported != all(report[k] >= 0 for k in ("device5_hresult", "context4_hresult", "create_fence_hresult")):
        raise ValueError("Fence availability contradicts the observed interfaces")
    rows = report["cases"]
    expected = set(itertools.product(range(4), range(36), range(3 if supported else 1)))
    if len(rows) != len(expected) or {(r["workload"], r["iteration"], r["order"]) for r in rows} != expected:
        raise ValueError("Incomplete interleaved completion matrix")
    for row in rows:
        mode = ("poll", "fence", "map-fence")[(row["order"] + row["iteration"]) % 3] if supported else "poll"
        if (row["mode"] != mode or row["warmup"] != (row["iteration"] < 4)
                or row["words"] != (64, 262144, 64, 4096)[row["workload"]]
                or row["seed"] != (0x12345678 ^ (row["iteration"] * 113 + row["workload"]))
                or not row["exact"] or not row["query_ready"]
                or row["maps"] < 1 or row["query_polls"] < 1 or row["sleeps"] < 0
                or row["first_map_hresult"] != (0 if row["maps"] == 1 else -2005270518)
                or row["maps"] != 1 + row["sleeps"] + int(mode == "map-fence" and row["maps"] > 1)
                or row["wait_calls"] < 0 or (mode == "poll" and row["wait_calls"] != 0)):
            raise ValueError("Completion case changed output, ordering or bounded retry evidence")
        if any(not math.isfinite(row[k]) or row[k] < 0 for k in ("wait_ms", "elapsed_ms", "cpu_ms")):
            raise ValueError("Invalid completion timing")
        if row["elapsed_ms"] < row["wait_ms"] or row["elapsed_ms"] >= 6000:
            raise ValueError("Completion exceeded its observation deadline")
    if supported:
        recovery = report["recovery"]
        if (any(not recovery[k] for k in ("timeout", "cancelled", "subsequent_completion",
                                          "submitted_cancellation", "registered_events_drained"))
                or recovery["cancel_checks"] != 2
                or not 0 <= recovery["timeout_ms"] < 1000
                or not 0 <= recovery["cancel_ms"] < 1000):
            raise ValueError("Missing bounded cancellation, timeout or event lifetime evidence")
    summaries = []
    for workload, mode in itertools.product(range(4), ("poll", "fence", "map-fence") if supported else ("poll",)):
        selected = [r for r in rows if not r["warmup"] and r["workload"] == workload and r["mode"] == mode]
        elapsed = sorted(r["elapsed_ms"] for r in selected)
        summaries.append(dict(workload=workload, mode=mode, samples=len(selected),
            median_ms=statistics.median(elapsed), p95_ms=elapsed[math.ceil(len(elapsed) * .95) - 1],
            sum_ms=sum(elapsed), cpu_ms=sum(r["cpu_ms"] for r in selected),
            readback_retries=sum(r["maps"] - 1 for r in selected),
            sleeps=sum(r["sleeps"] for r in selected), query_retries=sum(r["query_polls"] - 1 for r in selected)))
    return dict(vendor=vendor, adapter_luid=report["adapter_luid"], driver_version=report["driver_version"],
                fence_supported=supported, cases=len(rows), compared_words=sum(r["words"] for r in rows),
                recovery=report.get("recovery"), timings=summaries)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    for name in ("source", "binary", "evidence"):
        parser.add_argument("--" + name, type=Path, action="append", default=[])
    args = parser.parse_args()
    if len(args.root) < 2 or len(set(args.root)) != len(args.root):
        raise ValueError("Completion observations require distinct initial and repeat runs")
    runs = []
    for root in args.root:
        for vendor in (0, 4318, 32902):
            path = root / str(vendor) / "results.json"
            report = json.loads(path.read_text(encoding="utf-8"))
            if report["vendor"] != vendor:
                raise ValueError("Wrong adapter report path")
            runs.append(dict(report=identity(path), shader=identity(path.parent / "compute.dxbc"), **validate(report)))
    for vendor in (0, 4318, 32902):
        selected = [r for r in runs if r["vendor"] == vendor]
        if len({(r["adapter_luid"], r["driver_version"], r["fence_supported"]) for r in selected}) != 1:
            raise ValueError("Repeated observations changed the adapter or fence availability")
    if len({r["shader"]["sha256"] for r in runs}) != 1:
        raise ValueError("The independent compute program changed across observations")
    result = dict(schema=1, scope="independent_gpu_completion", validation_passed=True,
                  complete_release_acceptance=False, production_wait_changed=False, performance_acceptance=False,
                  cases=sum(r["cases"] for r in runs), compared_words=sum(r["compared_words"] for r in runs),
                  runs=runs, sources=[identity(p) for p in args.source],
                  binaries=[identity(p) for p in args.binary], evidence=[identity(p) for p in args.evidence])
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Validated {result['cases']} exact completion observations across {len(args.root)} serial runs")


if __name__ == "__main__":
    main()
