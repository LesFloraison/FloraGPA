"""Check private return counts, preserved controls and explicit count provenance."""
import argparse
import copy
import hashlib
import itertools
import math
import json
from pathlib import Path
import re
import struct


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def sha(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def verify(report, vendor, expected=None, alpha=None, component_type=3, depth_source=False, allow_unknown=False):
    require(report["schema"] == 1 and report["kind"] == ("native_depth_return_count_observation" if depth_source else "native_pixel_return_count_observation")
            and report["backend"] == (("native-depth-return-count/3" if "additional_target_shader" in report else "native-depth-return-count/2") if depth_source else "native-pixel-return-count/3")
            and report["scope"] == "returning_samples_on_private_additive_target", "Wrong scope")
    require(report["original_fragment_eligibility"] is None and report["original_gpu_schedule"] is False,
            "Unsupported original identity claim")
    require(report["adapter"]["vendor"] == (vendor or 5140) and report["adapter"]["warp"] == (vendor == 0)
            and report["adapter"]["device"] > 0, "Wrong adapter")
    require(report["debug_validation"] == "no_errors" and report["extra_uav_slots"] == 0, "Invalid execution")
    require(report["diagnostic_working_payload_bytes"] <= report["payload_limit"], "Payload exceeds budget")
    shader = report["completion_shader"]
    require(shader["component_type"] == component_type and shader["marker_bits"] == [0x3f800000] * 3
            and shader["backend"] == "native-pixel-return-count-shader/1"
            and shader["marker_precision"] == "full" and shader["alpha_storage_is_original_bits"] is False
            and shader["extra_uav_slots"] == 0 and shader["alpha_preserved"] is True
            and shader["requires_private_unblended_target"] is False
            and shader["requires_additive_rgb_float32_target"] is True, "Wrong shader contract")
    target = report["completion_target"]
    require(target["format"] == 2 and target["rgb_reduction"] == "add"
            and target["alpha_reduction"] == "replace" and target["cleared_to_zero"] is True
            and target["write_mask"] == 15 and target["logic_disabled"] is True, "Wrong accumulation state")
    transport = report["count_transport"]
    require(transport["slot"] == target["slot"] == shader["target"] and transport["view_format"] == 2
            and transport["layer_domain"] == "bound_rtv_view", "Wrong transport identity")
    require(type(transport["source_slot"]) is int and 0 <= transport["source_slot"] < 8
            and type(transport["auxiliary_target"]) is bool, "Missing source routing")
    require((transport["source_slot"] == 0 and transport["slot"] == 1) if transport["auxiliary_target"]
            else transport["source_slot"] == transport["slot"], "Invalid source routing")
    require(0 <= transport["sample"] < transport["samples"] and 0 <= transport["layer"] < transport["layers"]
            and 0 <= transport["x"] < transport["width"] and 0 <= transport["y"] < transport["height"], "Wrong selection")
    result = report["return_count"]
    bits = result["raw_bits"]
    require(len(bits) == 4 and all(type(x) is int and 0 <= x <= 0xffffffff for x in bits), "Invalid raw bits")
    raw = struct.unpack("<f", struct.pack("<I", bits[0]))[0]
    integral = math.isfinite(raw) and raw >= 0 and math.floor(raw) == raw
    valid = bits[:3] == [bits[0]] * 3 and integral and raw < 2**24
    reason = ('exact_float32_integer' if valid else 'rgb_disagreement' if bits[:3] != [bits[0]] * 3
              else 'invalid_accumulation' if not integral else 'float32_integer_limit')
    require(result['valid'] is valid and result['exclusive_integer_limit'] == 2**24
            and result['original_fragment_eligibility'] is None and result['reason'] == reason, 'Count validity')
    require((type(result['count']) is int and result['count'] == raw) if valid else
            (allow_unknown and result['count'] is None), 'Count decode')
    if expected is not None:
        require(valid and result["count"] == expected, "Count differs from owned geometry model")
    if alpha is not None:
        require(bits[3] == struct.unpack("<I", struct.pack("<f", alpha))[0], "Original alpha changed")
    require(type(report["passing_samples"]) is int and report["passing_samples"] >= 0, "Missing raw query")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    root = args.root
    observations = controls = 0
    query_differences = []
    frame_hashes = {}
    for vendor, layout, samples, mode, targets in itertools.product((0, 4318, 32902), range(3), (1, 4), range(4), (1, 8)):
        path = root / "native" / f"count-{vendor}-{layout}-{samples}-{mode}-{targets}.json"
        data = read(path)
        require([data[k] for k in ("vendor", "layout", "samples", "mode", "targets")]
                == [vendor, layout, samples, mode, targets], "Wrong owned selection")
        require(len(data["reports"]) == samples, "Incomplete sample observations")
        frame_hash = sha(path.with_suffix(".gpa_frame"))
        require(frame_hashes.setdefault((layout, samples, mode, targets), frame_hash) == frame_hash,
                "Owned frame changed between adapters")
        expected = 260 if mode == 0 else 130
        for sample, report in enumerate(data["reports"]):
            verify(report, vendor, expected, sample)
            require(report["frame_sha256"] == frame_hash and report["count_transport"]["sample"] == sample
                    and report["count_transport"]["slot"] == targets - 1, "Wrong sample/frame identity")
            observations += 1
            if mode == 0 and report["passing_samples"] != 260 * samples:
                query_differences.append({"vendor": vendor, "layout": layout, "samples": samples,
                                          "targets": targets, "sample": sample,
                                          "query": report["passing_samples"], "count": expected})
            for mutation in range(5):
                bad = copy.deepcopy(report)
                if mutation == 0: bad["return_count"]["raw_bits"][0] ^= 1
                if mutation == 1: bad["return_count"]["count"] += 1
                if mutation == 2: bad["completion_target"]["rgb_reduction"] = "maximum"
                if mutation == 3: bad["original_fragment_eligibility"] = True
                if mutation == 4: bad["count_transport"]["view_format"] = 28
                try:
                    verify(bad, vendor, expected, sample)
                except ValueError:
                    controls += 1
                else:
                    raise ValueError("Accepted corrupted count evidence")
    transport_modes = ("unorm", "srgb", "float16", "mip-array", "msaa-array", "alpha-zero",
                       "sample-mask", "disabled", "depth", "stencil", "write-mask")
    for vendor, mode in itertools.product((0, 4318, 32902), transport_modes):
        path = root / "native" / f"transport-{vendor}-{mode}.json"
        data = read(path)
        array = mode in ("mip-array", "msaa-array")
        samples = 4 if mode in ("msaa-array", "alpha-zero", "sample-mask") else 1
        selections = list(itertools.product(range(2 if array else 1), range(samples)))
        require(len(data["reports"]) == len(selections), "Incomplete transport observations")
        for (layer, sample), row in zip(selections, data["reports"]):
            expected = 0 if mode in ("disabled", "alpha-zero") or (mode == "sample-mask" and sample != 1) else (
                130 if array else 1 if mode in ("depth", "stencil") else 260)
            report = row["report"]
            verify(report, vendor, expected, 1 if expected else 0)
            t = report["count_transport"]
            require((t["layer"], t["sample"]) == (layer, sample) and t["source_mip"] == (mode == "mip-array")
                    and t["source_first_layer"] == int(array) and report["frame_sha256"] == sha(path.with_suffix(".gpa_frame")),
                    "Wrong subresource mapping")
            require(row["expected"] == expected, "Producer expectation differs")
            observations += 1
    for vendor, counter in itertools.product((0, 4318, 32902), (0, 1)):
        path = root / "native" / f"counters-{vendor}-{counter}.json"
        data = read(path)
        verify(data["report"], vendor, 1, 1)
        require(data["initial_counter"] == data["restored_counter"]
                and data["private_counter"] == data["initial_counter"] + 1, "Hidden counter was not isolated")
        require(data["report"]["frame_sha256"] == sha(path.with_suffix(".gpa_frame")), "Wrong counter frame")
        observations += 1
    for vendor in (0, 4318, 32902):
        data = read(root / "native" / f"recovery-{vendor}.json")
        require(data["observer_failures_restored"] == data["cancellation_points"] == 2
                and data["invalid_selections_rejected"] == 5, "Missing recovery controls")
        require(all(data[k] is True for k in ("cancellation_restored", "budget_restored", "timeout_restored",
                                             "integer_output_supported")), "Failed recovery")
        verify(data["retry"], vendor, 260, 0)
        require([r["skip"] for r in data["predicates"]] == [False, True], "Missing predicate controls")
        for row in data["predicates"]:
            verify(row["report"], vendor, 0 if row["skip"] else 1, 0 if row["skip"] else 1)
        observations += 3
    real = []
    for name, vendor in itertools.product(("gf2", "bf1", "helldivers"), (0, 4318, 32902)):
        directory = root / "real" / f"{name}-{vendor}"
        report = read(directory / "report.json")
        verify(report, vendor)
        require(report["probe"]["status"] == "ok" and report["probe"]["original_storage_restored"] is True
                and report["probe"]["count_is_original_fragment_identity"] is False, "Failed real-capture control")
        preserved = ("before", "cleared", "restored") if report["count_transport"]["auxiliary_target"] else (
            "before", "cleared", "marked", "restored")
        require(len({(directory / (n + ".bin")).read_bytes() for n in preserved}) == 1,
                "Selected original color storage changed")
        if report["count_transport"]["auxiliary_target"]:
            require((directory / "marked.bin").read_bytes() == (directory / "reference.bin").read_bytes(),
                    "Auxiliary draw changed original RT0 behavior")
        require((directory / "original-draw.bin").read_bytes() == (directory / "reference.bin").read_bytes(),
                "Original private draw differs from ordinary replay")
        require(sha(directory / "original.dxbc") == report["completion_shader"]["source_sha256"]
                and sha(directory / "marked.dxbc") == report["completion_shader"]["shader_sha256"], "Wrong shader bytes")
        real.append({"capture": name, "vendor": vendor, "count": report["return_count"]["count"],
                     "raw_query": report["passing_samples"], "elapsed_ms": report["probe"]["elapsed_ms"]})
    exits = read(root / "exits.json")
    require(len(exits) == 14 and all(x["exit_code"] == 0 for x in exits), "GPU process failed or was omitted")
    totals = []
    for run in exits[:5]:
        match = re.search(r"Totals: (\d+) passed, (\d+) failed, (\d+) skipped, (\d+) blacklisted",
                          (root / (run["label"] + ".txt")).read_text(encoding="utf-8"))
        require(match is not None and tuple(map(int, match.groups()[1:])) == (0, 0, 0), "Regression failed or skipped")
        totals.append({"suite": run["label"], "passed": int(match[1])})
    summary = {"schema": 1, "scope": "private_return_count_validation", "owned_observations": observations,
               "corruptions_rejected": controls, "original_fragment_eligibility": None,
               "raw_query_disagreements": query_differences, "real_capture_controls": real, "qt_totals": totals}
    args.out.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in summary.items() if k != "raw_query_disagreements"}, indent=2))


if __name__ == "__main__":
    main()
