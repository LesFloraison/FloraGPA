"""Compare saved native minimum-precision sampling results with independent D3D11 draws."""
import argparse
import copy
import hashlib
import json
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def identity(path):
    data = path.read_bytes()
    return {"path": str(path), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def verify(reference, observed):
    require(reference["scope"] == "independent_pixel_sampling", "Wrong reference scope")
    require(observed["scope"] == "native_pixel_sampling_execution", "Wrong execution scope")
    for key in ("schema", "vendor", "adapter_vendor", "adapter_device", "adapter_luid", "precision_scenario"):
        require(reference[key] == observed[key], "Changed identity: " + key)
    helper = observed["helper"]
    luid = ((helper["luid_high"] & 0xffffffff) << 32) | helper["luid_low"]
    require(helper["backend"] == "dxbc-gpu/18", "Wrong precision backend")
    require(helper["vendor_id"] == reference["adapter_vendor"] and
            helper["device_id"] == reference["adapter_device"] and luid == reference["adapter_luid"],
            "Different execution adapter")
    require(reference["unexpected_validation_errors"] == observed["unexpected_validation_errors"] == 0,
            "GPU validation error")
    require(len(reference["cases"]) == len(observed["cases"]) == 576, "Incomplete precision matrix")
    components = 0
    for expected, actual in zip(reference["cases"], observed["cases"]):
        require({k: v for k, v in expected.items() if k != "output"} ==
                {k: v for k, v in actual.items() if k not in ("output", "validity")},
                "Changed case identity or inputs")
        require(expected["status"] == "observed", "Undefined reference result")
        require(len(actual["output"]) == len(actual["validity"]) == 4, "Incomplete quad")
        active = expected["coverage"] & ~expected["discarded"]
        for lane in range(4):
            if active & (1 << lane):
                require(actual["validity"][lane] == 15, "Unknown active output")
                require(actual["output"][lane] == expected["output"][lane], "GPU output bits differ")
                components += 4
    require(components == 4464, "Active coverage count changed")
    return components


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    runs, artifacts, seen, controls = [], set(), set(), {}
    for path in sorted(args.directory.glob("precision-*")):
        reference = json.loads((path / "results.json").read_text())
        observed = json.loads((path / "native-results.json").read_text())
        key = reference["vendor"], reference["precision_scenario"]
        require(key not in seen, "Duplicate precision run")
        seen.add(key)
        components = verify(reference, observed)
        runs.append({"vendor": key[0], "scenario": key[1], "components": components, "path": str(path)})
        artifacts.update(p for p in path.iterdir() if p.is_file())
        for name in ("bits", "validity", "shader", "coverage", "scenario", "adapter", "missing_case"):
            changed = copy.deepcopy(observed)
            if name == "bits":
                changed["cases"][0]["output"][0][0] ^= 1
            elif name == "validity":
                changed["cases"][0]["validity"][0] = 14
            elif name == "shader":
                changed["cases"][0]["shader"] = "different.dxbc"
            elif name == "coverage":
                changed["cases"][0]["coverage"] = 0
            elif name == "scenario":
                changed["precision_scenario"] = 0
            elif name == "adapter":
                changed["helper"]["luid_low"] ^= 1
            else:
                changed["cases"].pop()
            try:
                verify(reference, changed)
            except ValueError as error:
                controls[f"{key[0]}-{key[1]}-{name}"] = str(error)
            else:
                raise ValueError("Corruption accepted: " + name)
    require(seen == {(v, s) for v in (0, 4318, 32902) for s in range(1, 7)}, "Missing precision run")
    report = {"schema": 1, "runs": runs, "cases": len(runs) * 576,
              "exact_components": sum(r["components"] for r in runs), "negative_controls": controls,
              "artifacts": [identity(p) for p in sorted(artifacts)]}
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
    print(json.dumps({"runs": len(runs), "cases": report["cases"],
                      "exact_components": report["exact_components"], "negative_controls": len(controls)}))


if __name__ == "__main__":
    main()
