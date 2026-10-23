"""Compare saved migration reports; does not load or launch any external backend.

Only original GPA identities can be matched. All unmatched rows and catalog entries
are retained in the output. Durations are independent measurements, not exact values.
"""
import argparse
import json
import math
from pathlib import Path

from validate_native_analysis import digest, load

# Explicit migration mapping, isolated from the product's project-owned identifiers.
COUNTERS = {
    1: "gpu.duration", 2: "input.vertices", 3: "input.primitives",
    4: "gs.primitives", 5: "clipper.invocations", 6: "clipper.primitives",
    7: "samples.passed", 8: "vs.invocations", 9: "hs.invocations",
    10: "ds.invocations", 11: "gs.invocations", 12: "ps.invocations",
    13: "cs.invocations",
}


def compare(native, reference):
    values = {}
    for row in native["data"]["events"]:
        for counter, value in row["values"].items():
            key = (row["event"], counter)
            if key in values:
                raise ValueError("Duplicate native counter identity")
            values[key] = value
    matched, differences, durations, unmapped, missing = [], [], [], [], []
    seen = set()
    for row in reference["values"]:
        if row["gpa_event"] is None:
            unmapped.append(row)
            continue
        counter = COUNTERS.get(row["counter"])
        key = (row["gpa_event"], counter)
        if counter is None or key not in values:
            missing.append(row)
            continue
        if key in seen:
            raise ValueError("Multiple reference samples map to one GPA counter")
        seen.add(key)
        entry = {"event": key[0], "counter": counter,
                 "native": values[key], "reference": row["value"]}
        expected_unit = "CounterUnit.Seconds" if counter == "gpu.duration" else "CounterUnit.Absolute"
        if row["unit"] != expected_unit:
            raise ValueError("Reference unit disagrees with migration mapping")
        if counter == "gpu.duration":
            entry["valid"] = all(type(value) in (float, int) and math.isfinite(value) and value >= 0
                                 for value in (values[key], row["value"]))
            durations.append(entry)
        elif type(values[key]) is not int or type(row["value"]) is not int or values[key] != row["value"]:
            differences.append(entry)
        else:
            matched.append(entry)
    return {
        "mapped_values_match": not differences and not missing and bool(seen)
                               and all(row["valid"] for row in durations),
        "exact_integer_values": len(matched), "differences": differences,
        "independent_durations": durations, "missing_native_values": missing,
        "reference_without_gpa_identity": unmapped,
        "additional_native_values": [{"event": event, "counter": counter, "value": value}
                                     for (event, counter), value in values.items() if (event, counter) not in seen],
        "native_catalog": native["data"]["catalog"], "reference_catalog": reference["available"],
        "scope": "Mapped generic counter values only; not complete metadata, performance or release acceptance",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    result = compare(load(args.native), load(args.reference))
    result["inputs"] = {key: {"path": str(path.resolve()), "sha256": digest(path)}
                        for key, path in (("native", args.native), ("reference", args.reference))}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps({key: result[key] for key in ("mapped_values_match", "exact_integer_values")}))
    print("Differences:", len(result["differences"]), "missing:", len(result["missing_native_values"]))
    return 0 if result["mapped_values_match"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
