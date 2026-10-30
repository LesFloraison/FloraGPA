"""Verify private GPA stencil observations without accepting known selection deviations."""
import argparse
import copy
import hashlib
import itertools
import json
import struct
import sys
from pathlib import Path

from validate_pixel_ordinals import RANKS, expected_fragment, require


def bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def values(layout, sample, fragment, floating):
    result = [fragment + 1, 7 if layout == 2 else 0 if layout == 1 else fragment,
              fragment if layout == 1 else 0, sample]
    return [bits(v) for v in result] if floating else result


def native(report, key):
    vendor, layout, samples, mode, targets = key
    require([report[k] for k in ("vendor", "layout", "samples", "mode", "targets")] == list(key),
            "Native selection identity differs")
    require(report["before"] == [[bits(v) for v in (.125, .5, .25, .25)]] * samples,
            "Unexpected initial storage")
    requests = list(itertools.product((0, 1, 2, 127, 253, 254), (False, True)))
    require(len(report["rows"]) == len(requests), "Incomplete native observations")
    errors = []
    for row, (ordinal, marked) in zip(report["rows"], requests):
        result = row["report"]
        selection = result["ordinal_selection"]
        require(result["schema"] == 1 and result["backend"] == "native-pixel-ordinal/1" and
                result["scope"] == "original_ps_body_with_counterfactual_stencil_reference", "Wrong native scope")
        require(result["original_fragment_eligibility"] is None and not result["original_gpu_schedule"] and
                selection["verified_fragment_ordinal"] is None and
                selection["original_depth_stencil_eligibility"] is None, "Unproven qualification")
        require(selection["requested_ordinal"] == ordinal and selection["mark_returns"] == marked and
                selection["target"] == targets - 1 and selection["maximum_supported_ordinal"] == 254,
                "Wrong stencil selection")
        require(result["adapter"]["vendor"] == (vendor or 5140) and result["adapter"]["warp"] == (vendor == 0),
                "Wrong native adapter")
        require(selection["force_early_depth_stencil"] == (mode == 2) and selection["population"] ==
                ("early_test_stencil_updates" if mode == 2 else "late_test_survivor_updates"), "Wrong population")
        require(selection["stencil_payload_bytes"] == 4 * samples and selection["width"] ==
                selection["height"] == selection["layer_count"] == 1 and selection["mip"] ==
                selection["first_layer"] == 0 and selection["samples"] == samples, "Wrong stencil layout")
        require(selection["application_uav_bindings_retained"] and result["extra_uav_slots"] == 0 and
                result["diagnostic_shader_replacements"] == marked, "Wrong shader or UAV contract")
        require(result["diagnostic_working_payload_bytes"] <= result["payload_limit"], "Payload exceeded")
        require(result["debug_validation"] == "no_errors", "Native validation error")
        expected = []
        for sample in range(samples):
            fragment = expected_fragment(mode, sample, 260, ordinal)
            value = report["before"][sample] if fragment is None else values(layout, sample, fragment, True)
            if marked:
                value = [0, 0, 0, 0] if fragment is None else [bits(v) for v in (1, 2, 3, sample)]
            expected.append(value)
        require(row["expected"] == expected, "Producer expectation differs from independent model")
        require(len(row["actual"]) == samples and all(len(v) == 4 for v in row["actual"]), "Missing sample bits")
        if row["actual"] != expected:
            errors.append({"ordinal": ordinal, "marked": marked, "actual": row["actual"], "expected": expected})
    return errors


def independent(root, vendors, extent, floating, raster, mip):
    errors = []
    groups = 0
    for vendor, layout in itertools.product(vendors, range(3)):
        key = (vendor, layout, 1, 3, 8)
        path = root / "-".join(map(str, key)) / "results.json"
        report = json.loads(path.read_text(encoding="utf-8"))
        require([report[k] for k in ("requested_vendor", "layout", "samples", "mode", "targets")] == list(key),
                "Independent identity differs")
        require(report["actual_vendor"] == (vendor or 5140) and not report["diagnostics"], "Independent device error")
        require(report["extent"] == extent and report["float_output"] == floating and
                report["raster_multisample"] == raster and report.get("stencil_mip", 0) == mip,
                "Independent configuration differs")
        require([(r["count"], r["rank"]) for r in report["rows"]] == list(itertools.product((8, 260), RANKS)),
                "Independent sequence differs")
        failures = []
        for row in report["rows"]:
            fragment = expected_fragment(3, 0, row["count"], row["rank"])
            expected = [[0, 0, 0, 0] if fragment is None else values(layout, 0, fragment, floating)]
            require(row["expected"] == expected, "Independent probe expectation differs")
            if row["outputs"] != expected:
                failures.append({"count": row["count"], "rank": row["rank"], "actual": row["outputs"],
                                 "expected": expected})
        if failures:
            errors.append({"key": key, "rows": failures})
        groups += 1
    return {"groups": groups, "failures": errors}


REAL = {
    "gf2": (113, 80, 102, 799, 480, "fc7b7f7de20a578da2977b2ff91d96f5cf80887b393bc85859be8cab1af91c8e"),
    "bf1": (2010, 954, 1992, 751, 433, "26044d33cb6c3087c2204b7d57664203741a679f988430b7d05b24af582e0620"),
    "helldivers": (200, 99, 168, 168, 183, "97915a80c7617e9a67068a5d85b87f1b2a7883faa9831428084ebd7270f37139"),
}


def real(report, files, name, vendor):
    event, resource, shader, x, y, digest = REAL[name]
    require(report["backend"] == "native-pixel-ordinal/1" and report["scope"] ==
            "original_ps_body_with_counterfactual_stencil_reference", "Wrong real observation scope")
    require(report["event"] == event and report["frame_sha256"] == digest and report["selection"] ==
            {"resource": resource, "shader": shader, "x": x, "y": y, "slot": 0, "sample": 0},
            "Wrong real frame or selection")
    require(report["adapter"]["vendor"] == (vendor or 5140) and report["adapter"]["warp"] == (vendor == 0),
            "Wrong real adapter")
    selection = report["ordinal_selection"]
    require(selection["verified_fragment_ordinal"] is None and selection["original_depth_stencil_eligibility"]
            is None and report["original_fragment_eligibility"] is None and not report["original_gpu_schedule"],
            "Real observation inferred original identity")
    require(selection["requested_ordinal"] == selection["target"] == 0 and selection["mark_returns"] and
            selection["application_uav_bindings_retained"] and report["extra_uav_slots"] == 0,
            "Wrong real stencil selection")
    require(report["diagnostic_working_payload_bytes"] <= report["payload_limit"] and
            report["debug_validation"] == "no_errors", "Real diagnostic budget or validation failed")
    require(report["probe"]["status"] == "ok" and report["probe"]["original_storage_restored"] and
            files["before.bin"] == files["restored.bin"] and files["original-draw.bin"] == files["reference.bin"]
            and bool(files["before.bin"]), "Real restoration or original control differs")
    control = report["original_draw"]
    require(control["event"] == event and control["frame_sha256"] == digest and
            control["adapter"] == report["adapter"] and control["debug_validation"] == "no_errors" and
            control["passing_samples"] == report["probe"]["reference_passing_samples"], "Real control differs")
    for filename, field in (("original.dxbc", "source_sha256"), ("marked.dxbc", "shader_sha256")):
        require(hashlib.sha256(files[filename]).hexdigest() == report["completion_shader"][field],
                "Real shader evidence differs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    root = args.root
    keys = list(itertools.product((0, 4318, 32902), range(3), (1, 4), range(4), (1, 8)))
    failures = []
    controls = 0
    for key in keys:
        report = json.loads((root / "final" / ("extraction-" + "-".join(map(str, key)) + ".json")).read_text(encoding="utf-8"))
        errors = native(report, key)
        if errors:
            failures.append({"key": key, "rows": errors})
        # Synthetic controls test the validator; the observed files are never rewritten.
        canonical = copy.deepcopy(report)
        for row in canonical["rows"]:
            row["actual"] = row["expected"]
        require(not native(canonical, key), "Invalid control model")
        for change in range(6):
            bad = copy.deepcopy(canonical)
            first = bad["rows"][0]
            if change == 0:
                first["report"]["ordinal_selection"]["verified_fragment_ordinal"] = 0
            elif change == 1:
                first["report"]["ordinal_selection"]["requested_ordinal"] = 1
            elif change == 2:
                first["report"]["ordinal_selection"]["force_early_depth_stencil"] = not (key[3] == 2)
            elif change == 3:
                first["report"]["adapter"]["vendor"] = 12345
            elif change == 4:
                bad["rows"].pop()
            else:
                first["actual"] = copy.deepcopy(first["actual"])
                first["actual"][0][0] ^= 1
            try:
                detected = bool(native(bad, key))
            except ValueError:
                detected = True
            require(detected, "Altered observation accepted")
            controls += 1
    bindings = []
    for vendor, mode in itertools.product((0, 4318, 32902),
                                          ("append", "counter", "classes", "predicate_pass", "predicate_skip", "depth")):
        path = root / "final" / f"bindings-{vendor}-{mode}.json"
        report = json.loads(path.read_text(encoding="utf-8"))
        require(report["actual"] == report["reference"] and report["restored"], "Binding recovery failed")
        require(report["report"]["ordinal_selection"]["verified_fragment_ordinal"] is None,
                "Binding probe inferred ordinal")
        counter = mode in ("append", "counter")
        require(report["before_counter"] == 0 and report["counter"] == int(counter) and
                report["buffer"] == ([7] + [0] * 15 if counter else []), "Binding side effects differ")
        require(report["actual"] == ([0, 255, 0, 255] if mode == "predicate_pass" else [255, 0, 0, 255]),
                "Binding output differs")
        bindings.append(path.name)
    real_rows = []
    for name, vendor in itertools.product(REAL, (0, 4318, 32902)):
        directory = root / "real" / f"{name}-{vendor}"
        report = json.loads((directory / "report.json").read_text(encoding="utf-8"))
        files = {p.name: p.read_bytes() for p in directory.iterdir() if p.suffix in (".bin", ".dxbc")}
        real(report, files, name, vendor)
        for change in range(4):
            bad, changed = copy.deepcopy(report), dict(files)
            if change == 0:
                bad["ordinal_selection"]["verified_fragment_ordinal"] = 0
            elif change == 1:
                bad["adapter"]["vendor"] = 12345
            else:
                filename = "before.bin" if change == 2 else "original.dxbc"
                data = bytearray(changed[filename])
                data[0] ^= 1
                changed[filename] = bytes(data)
            try:
                real(bad, changed, name, vendor)
            except ValueError:
                controls += 1
            else:
                raise ValueError("Altered real control accepted")
        real_rows.append({"name": name, "vendor": vendor, "elapsed_ms": report["probe"]["elapsed_ms"],
                          "passing_samples": report["passing_samples"],
                          "original_passing_samples": report["original_draw"]["passing_samples"],
                          "marked_storage": files["marked.bin"].hex(),
                          "verified_fragment_ordinal": None})
    probes = {name: independent(root / name, vendors, extent, floating, raster, mip)
              for name, vendors, extent, floating, raster, mip in (
                  ("independent-float", (0, 4318, 32902), 1, True, False, 0),
                  ("independent-raster-msaa", (32902,), 1, True, True, 0),
                  ("independent-float-four", (32902,), 4, True, True, 0),
                  ("independent-uint-one", (32902,), 1, False, True, 0),
                  ("independent-stencil-mip", (32902,), 1, True, False, 2))}
    result = {"schema": 1, "scope": "counterfactual_stencil_reference_observations",
              "complete_release_acceptance": False, "native_groups": len(keys), "native_draws": len(keys) * 12,
              "sample_vectors": sum(k[2] * 12 for k in keys), "binding_groups": len(bindings),
              "real_control_checks": real_rows,
              "corruption_controls": controls, "native_selection_failures": failures, "independent_probes": probes,
              "strict_contract_passed": not failures and not any(p["failures"] for p in probes.values()),
              "artifacts": [{"path": p.as_posix(), "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                            for directory in (root / "final", root / "real", *(root / name for name in probes))
                            for p in sorted(directory.rglob("*")) if p.is_file()]}
    with args.out.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps({k: v for k, v in result.items() if k not in
                     ("native_selection_failures", "independent_probes", "real_control_checks", "artifacts")}))
    return int(not result["strict_contract_passed"])


if __name__ == "__main__":
    sys.exit(main())
