"""Check native PS output observations against saved independent D3D11 results."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path

from validate_typed_load_precision import require, shader_rows


def operands(row):
    require(not row[0] >> 31, "Unexpected opcode extension in fixture")
    result, cursor = [], 1
    while cursor < len(row):
        token = row[cursor]
        cursor += 1
        precision = 0
        if token >> 31:
            extension = row[cursor]
            cursor += 1
            require(extension & ~0x1c000 == 1, "Unexpected operand extension")
            precision = extension >> 14 & 7
        dimensions = token >> 20 & 3
        require(token >> 22 & 0x1ff == 0, "Non-immediate fixture register index")
        indices = row[cursor:cursor + dimensions]
        require(len(indices) == dimensions, "Truncated operand")
        cursor += dimensions
        result.append((token & 0x7fffffff, precision, indices))
    return result


def constants(precision, dataset):
    result = [[0x3eaaaaab, 0xbf000000, 0x3f801001, 0x80000000],
              [0x3f000000, 0x3e800000, 0x3f000001, 0x3eaaaaab],
              [0x3e000000, 0x3f000000, 0xbf000000, 0],
              [0x3e800000, 0x3f400000, 0x3f000000, 0]] if precision < 4 else [
                  [0, 17, 0xffffffed if precision == 4 else 19, 127],
                  [7, 15, 23, 5], [31, 37, 41, 43], [47, 53, 59, 61]]
    if dataset:
        special = [[0, 0x80000000, 0x38800000, 0xb8800000],
                   [0x7fc12345, 0x7f800000, 0xff800000, 0x7f812345],
                   [1, 0x80000001, 0x007fffff, 0x807fffff]] if precision < 4 else [
                       [0, 32767, 32768, 65535], [65536, 0xffffffff, 0x80000000, 0x7fffffff],
                       [17, 0xffffffef, 0, 1]]
        result[0] = special[dataset - 1]
    return result


def verify(report, shader):
    require(report["scope"] == "native_pixel_output_precision", "Wrong output scope")
    require(hashlib.sha256(shader).hexdigest() == report["shader_sha256"], "Changed shader")
    require(report["valid"] == 15, "Unknown output")
    require(len(report["actual"]) == len(report["expected"]) == 4, "Incomplete output")
    require(report["actual"] == report["expected"], "Output bits differ")
    precision, slot, mask = report["precision"], report["slot"], report["mask"]
    require(precision in (1, 2, 4, 5), "Invalid output precision")
    require(report["constants"] == constants(precision, report["dataset"]), "Changed input constants")
    helper = report["helper"]
    require(helper["backend"] == "dxbc-gpu/20", "Wrong output backend")
    require(helper["warp"] == (report["vendor"] == 0), "Wrong adapter selection")
    require(helper["vendor_id"] == (report["vendor"] or 5140), "Wrong adapter vendor")
    require(all(helper[key] == value for key, value in report["reference_adapter"].items()),
            "Different reference adapter")
    stage, rows = shader_rows(shader)
    require(stage == 0 and len(rows) == (6 if mask == 15 else 7), "Wrong program shape")
    require(rows[0][1] == (0x101006a | (0x800 if report["refactoring_allowed"] else 0),),
            "Wrong global flags")
    require(rows[1][1] == (0x4000059, 0x208e46, 0, 4), "Wrong constant declaration")
    require(rows[2][1][0] & 2047 == 101 and
            operands(rows[2][1]) == [(0x1020f2, precision, (slot,))], "Wrong output declaration")
    require(rows[3][1] == (0x2000068, 1), "Wrong temporary declaration")
    if mask != 15:
        initial = rows[4][1]
        require(initial[0] & 0xffffff == 54 and operands(initial) == [
            (0x102002 | ((15 ^ mask) << 4), precision, (slot,)), (0x208e46, 0, (0, 3))],
            "Wrong partial output initialization")
    operation = rows[-2][1]
    require(operation[0] & 0xffffff == report["opcode"], "Changed output operation")
    args = operands(operation)
    count = 1 if report["opcode"] & 2047 in (54, 75) else 3 if report["opcode"] in (50, 55) else 2
    require(args == [(0x102002 | (mask << 4), precision, (slot,))] + [
        (0x208e46, precision if report["source_precision"] else 0, (0, i)) for i in range(count)],
        "Changed output operands")
    require(rows[-1][1] == (0x100003e,), "Missing return")
    return 4


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    seen, controls, artifacts, selected = set(), {}, [], set()
    components = 0
    for path in sorted(args.directory.glob("*.json")):
        report = json.loads(path.read_text())
        key = tuple(report[k] for k in ("vendor", "precision", "opcode", "slot", "mask",
                                       "source_precision", "refactoring_allowed", "dataset"))
        require(key not in seen, "Duplicate output case")
        seen.add(key)
        shader = path.with_suffix(".dxbc").read_bytes()
        components += verify(report, shader)
        for artifact in (path, path.with_suffix(".dxbc")):
            data = artifact.read_bytes()
            artifacts.append({"path": str(artifact), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        representative = key[:2]
        if representative in selected:
            continue
        selected.add(representative)
        for name in ("bits", "validity", "adapter", "shader", "precision", "slot", "mask", "constants", "flag"):
            changed = copy.deepcopy(report)
            if name == "bits": changed["actual"][0] ^= 1
            elif name == "validity": changed["valid"] = 14
            elif name == "adapter": changed["reference_adapter"]["luid_low"] ^= 1
            elif name == "shader": changed["shader_sha256"] = "changed"
            elif name == "precision": changed["precision"] = 0
            elif name == "slot": changed["slot"] ^= 1
            elif name == "mask": changed["mask"] ^= 1
            elif name == "constants": changed["constants"][0][0] ^= 1
            else: changed["refactoring_allowed"] = not changed["refactoring_allowed"]
            try:
                verify(changed, shader)
            except ValueError as error:
                controls[f"{key[0]}-{key[1]}-{name}"] = str(error)
            else:
                raise ValueError("Corruption accepted: " + name)
    expected = set()
    for vendor, precision in itertools.product((0, 4318, 32902), (1, 2, 4, 5)):
        operations = (54, 0, 56, 50, 16, 75, 54 | 0x2000) if precision < 4 else (
            54, 30, 36 if precision == 4 else 83, 37 if precision == 4 else 84, 55)
        for tail in itertools.product(operations, (0, 3, 7), (15, 5, 10), (False, True), (False, True), range(4)):
            if tail[-1] and (tail[0] != 54 or tail[2] != 15):
                continue
            expected.add((vendor, precision, *tail))
    require(seen == expected, "Incomplete output matrix")
    result = {"schema": 1, "scope": "same_adapter_pixel_output_observations", "cases": len(seen),
              "exact_components": components, "negative_controls": controls, "artifacts": artifacts}
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps({"cases": len(seen), "exact_components": components, "negative_controls": len(controls)}))


if __name__ == "__main__":
    main()
