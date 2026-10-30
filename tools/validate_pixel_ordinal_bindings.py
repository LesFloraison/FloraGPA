"""Independently verify binding observations without accepting occlusion discrepancies."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path
import re

from validate_pixel_ordinals import RANKS, expected_fragment, require, signatures, validate, query_failures

MASKS = (255, 128, 192, 240)
KEYS = list(itertools.product((0, 4318, 32902), range(3), (1, 4)))


def check(report, key, mask):
    vendor, layout, samples = key
    require(report["schema"] == 1 and report["scope"] == "owned_draw_stencil_ordinal_probe", "Wrong scope")
    require(report["original_fragment_eligibility"] is None, "Unproven original eligibility")
    require([report[k] for k in ("requested_vendor", "layout", "samples", "mode", "targets")]
            == [vendor, layout, samples, 0, 8], "Wrong case identity")
    require(report["actual_vendor"] == (vendor or 5140) and report["actual_device"] > 0
            and len(report["adapter_luid"]) == 2, "Wrong adapter identity")
    require(report["render_target_mask"] == report["observed_render_target_mask"] == mask, "Wrong bindings")
    require(report["repetitions"] == 4 and report["extent"] == 4 and not report["float_output"], "Wrong mode")
    require(report["stencil_format"] == 45 and report["stencil_mip"] == 0
            and report["raster_multisample"] is True and report["coverage_discard"] == 0
            and report["coverage_normalization"] is None, "Unexpected pipeline override")
    require([(r["count"], r["rank"], r["repetition"]) for r in report["rows"]]
            == list(itertools.product((8, 260), RANKS, range(4))), "Incomplete observation order")
    for message in report["diagnostics"]:
        slot = re.search(r"slot ([0-7]),", message["text"])
        require(message["id"] == 3146081 and message["severity"] == 2 and slot is not None
                and not mask & (1 << int(slot[1])) and message["expected_unbound_output_warning"] is True,
                "Unexpected GPU diagnostic")
    failures = []
    unstable = []
    for row in report["rows"]:
        count, rank = row["count"], row["rank"]
        expected = []
        for sample in range(samples):
            fragment = expected_fragment(0, sample, count, rank)
            expected.append([0] * 4 if fragment is None else
                            [fragment + 1, 7 if layout == 2 else 0 if layout == 1 else fragment,
                             fragment if layout == 1 else 0, sample])
        require(row["outputs"] == expected == row["expected"], "Wrong selected output")
        require(type(row["occlusion"]) is int and row["occlusion"] >= 0, "Invalid query value")
        passing = count if rank == -1 else max(count - 255, 0) if rank == 255 else int(rank < count)
        expected_count = 16 * samples * passing
        if row["occlusion"] != expected_count:
            failures.append({"count": count, "rank": rank, "repetition": row["repetition"],
                             "actual": row["occlusion"], "expected": expected_count})
    for count, rank in itertools.product((8, 260), RANKS):
        values = [r["occlusion"] for r in report["rows"] if (r["count"], r["rank"]) == (count, rank)]
        if len(set(values)) > 1:
            unstable.append({"count": count, "rank": rank, "occlusion": values})
    return failures, unstable


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--prior-root", type=Path)
    args = parser.parse_args()
    rows, controls = [], 0
    shaders = {}
    identities = {}
    vectors = 0
    prior = None if args.prior_root is None else {
        "configurations": 0, "query_failures": 0, "repeat_disagreements": 0, "query_value_changes": []}
    for mask in MASKS:
        require({p.name for p in (args.root / f"mask-{mask}").iterdir() if p.is_dir()}
                == {"-".join(map(str, (*key, 0, 8))) for key in KEYS}, "Wrong binding matrix")
    default_keys = list(itertools.product((0, 4318, 32902), range(3), (1, 4), range(4), (1, 8)))
    require({p.name for p in (args.root / "default").iterdir() if p.is_dir()}
            == {"-".join(map(str, key)) for key in default_keys}, "Wrong default matrix")
    for mask, key in itertools.product(MASKS, KEYS):
        tag = "-".join(map(str, (*key, 0, 8)))
        directory = args.root / f"mask-{mask}" / tag
        report = json.loads((directory / "results.json").read_text(encoding="utf-8"))
        failures, unstable = check(report, key, mask)
        vectors += len(report["rows"]) * key[2]
        identity = [report[k] for k in ("actual_vendor", "actual_device", "adapter_luid")]
        require(identities.setdefault(key[0], identity) == identity, "Adapter changed between observations")
        for name in ("vs", "gs", "ps", "readback"):
            digest = hashlib.sha256((directory / (name + ".dxbc")).read_bytes()).hexdigest()
            require(shaders.setdefault((key, name), digest) == digest, "Shader changed with bindings")
        if prior is not None:
            old_directory = args.prior_root / f"mask-{mask}" / tag
            old = json.loads((old_directory / "results.json").read_text(encoding="utf-8"))
            old_failures, old_unstable = check(old, key, mask)
            require([old[k] for k in ("actual_vendor", "actual_device", "adapter_luid")] == identity,
                    "Prior adapter changed")
            for name in ("vs", "gs", "ps", "readback"):
                require(hashlib.sha256((old_directory / (name + ".dxbc")).read_bytes()).hexdigest()
                        == shaders[key, name], "Prior shader changed")
            prior["configurations"] += 1
            prior["query_failures"] += len(old_failures)
            prior["repeat_disagreements"] += len(old_unstable)
            for before, after in zip(old["rows"], report["rows"]):
                if before["occlusion"] != after["occlusion"]:
                    prior["query_value_changes"].append({"key": list(key), "mask": mask,
                        "count": after["count"], "rank": after["rank"], "repetition": after["repetition"],
                        "prior": before["occlusion"], "current": after["occlusion"]})
        sig = signatures(directory / "ps.dxbc")[b"OSGN"]
        outputs = sorted(index for (semantic, index) in sig if semantic.lower() == "sv_target")
        require(outputs == list(range(8)), "PS does not declare all eight targets")
        for name, value in (("render_target_mask", 0), ("observed_render_target_mask", 0),
                            ("repetitions", 3), ("original_fragment_eligibility", True)):
            broken = copy.deepcopy(report)
            broken[name] = value
            try:
                check(broken, key, mask)
            except ValueError:
                controls += 1
            else:
                raise ValueError("Accepted corrupted " + name)
        for kind in ("output", "order", "warning"):
            broken = copy.deepcopy(report)
            if kind == "output":
                broken["rows"][0]["outputs"][0][0] ^= 1
            elif kind == "order":
                broken["rows"][0]["repetition"] = 3
            else:
                broken["diagnostics"].append({"id": 3146081, "severity": 2,
                    "text": "slot 7,", "expected_unbound_output_warning": True})
            try:
                check(broken, key, mask)
            except ValueError:
                controls += 1
            else:
                raise ValueError("Accepted corrupted " + kind)
        rows.append({"mask": mask, "key": list(key), "query_failures": failures,
                     "repeat_disagreements": unstable, "diagnostics": len(report["diagnostics"])})
    defaults = []
    for key in default_keys:
        path = args.root / "default" / "-".join(map(str, key)) / "results.json"
        report = json.loads(path.read_text(encoding="utf-8"))
        vectors += validate(report, key)
        failures = query_failures(report)
        defaults.append({"key": list(key), "query_failures": failures})
    exits = json.loads((args.root / "exits.json").read_text(encoding="utf-8"))
    require([x["label"] for x in exits] == [*(f"mask-{m}" for m in MASKS), "default"], "Incomplete processes")
    totals = []
    for run in exits:
        match = re.search(r"Totals: (\d+) passed, (\d+) failed, (\d+) skipped, (\d+) blacklisted",
                          (args.root / (run["label"] + ".txt")).read_text(encoding="utf-8"))
        require(match is not None, "Missing terminal Qt result")
        passed, failed, skipped, blacklisted = map(int, match.groups())
        expected_failed = sum(bool(r["query_failures"]) for r in defaults) if run["label"] == "default" else sum(
            bool(r["query_failures"]) for r in rows if run["label"] == f'mask-{r["mask"]}')
        total = 146 if run["label"] == "default" else 20
        require(not skipped and not blacklisted and failed == expected_failed and passed + failed == total
                and run["exit_code"] == failed, "Qt failures differ from preserved query discrepancies")
        totals.append({"label": run["label"], "passed": passed, "failed": failed, "skipped": skipped})
    failure_count = sum(len(r["query_failures"]) for r in rows + defaults)
    result = {"schema": 1, "scope": "owned_mrt_binding_experiment", "semantic_pass": failure_count == 0,
              "original_fragment_eligibility": None, "binding_configurations": len(rows),
              "default_configurations": len(defaults), "output_vectors_verified": vectors,
              "corruption_controls_rejected": controls, "query_failures": failure_count,
              "prior_comparison": prior,
              "adapters": identities, "qt_totals": totals, "binding_results": rows, "default_results": defaults}
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items()
                      if k not in ("binding_results", "default_results", "prior_comparison")}, indent=2))
    return int(failure_count != 0)


if __name__ == "__main__":
    raise SystemExit(main())
