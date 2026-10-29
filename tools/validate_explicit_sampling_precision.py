"""Audit explicit LOD/comparison precision against retained original GPU executions."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path
import struct

from validate_feedback_relocation import instructions, require
from validate_native_clamp_feedback import operand_end


def verify(folder, baseline, report=None):
    vendor, stage, comparison, feedback, precision, scenario = map(int, folder.name.split("-"))
    report = report if report is not None else json.loads((folder / "report.json").read_text("utf-8"))
    expected = [precision if scenario == 4 or (scenario == 1 and not comparison) else 0,
                precision if scenario in (2, 4) else 0, precision if scenario in (3, 4) else 0]
    opcode = (229 if comparison else 228) if feedback else (71 if comparison else 72)
    require(report["stage"] == stage and report["comparison"] is bool(comparison) and
            report["feedback"] is bool(feedback) and report["opcode"] == opcode and
            report["precision"] == expected, "Case identity and expected operand precision")
    require(report["adapter"]["vendor_id"] == (vendor or 5140) and
            report["adapter"]["warp"] is (vendor == 0) and report["adapter"]["feature_level"] == 0xb100,
            "Explicit adapter selection")
    shader = (folder / "shader.dxbc").read_bytes()
    require(shader == (baseline / folder.name / "shader.dxbc").read_bytes(), "Unchanged original shader")
    original = json.loads((folder / "original.json").read_text("utf-8"))
    require(original == json.loads((baseline / folder.name / "original.json").read_text("utf-8")),
            "Unchanged original GPU result")
    require(original == report["original"] == report["native"] and len(original) == 4 and
            all(len(row) == 4 and all(type(v) is int and 0 <= v <= 0xffffffff for v in row) for row in original),
            "Exact raw result vectors")
    require(len({tuple(row) for row in original}) > 1, "Original GPU observes different LOD/reference values")
    require(report["reversed"] == list(reversed(original)), "Cached shader consumes changed parameters")
    require(report["unknown_validity"] == [0, 0], "Unknown parameter and coordinate propagation")
    rows = [row for row in instructions(folder / "shader.dxbc") if row[0] & 2047 in (71, 72, 228, 229)]
    require(len(rows) == 1 and rows[0][0] & 2047 == opcode, "One requested sampling instruction")
    row = rows[0]
    pos = 1
    while row[pos - 1] >> 31:
        pos += 1
    annotations = []
    for _ in range(5 + feedback):
        start = pos
        pos = operand_end(row, pos)
        if row[start] >> 31:
            require(row[start + 1] & 63 == 1 and not row[start + 1] >> 31, "Single operand modifier")
        annotations.append((row[start + 1] >> 14) & 7 if row[start] >> 31 else 0)
    require([annotations[0], annotations[1 + feedback], annotations[4 + feedback]] == expected,
            "Actual DXBC precision matches scenario")
    require(annotations[2 + feedback] == annotations[3 + feedback] == 0 and
            (not feedback or annotations[1] == 0), "Full-precision bindings and opaque status")
    require(row[pos:] in ([], [0]), "Only observed compiler padding")
    count = struct.unpack_from("<I", shader, 28)[0]
    for offset in struct.unpack_from("<" + "I" * count, shader, 32):
        if shader[offset:offset + 4] in (b"SHDR", b"SHEX"):
            require(struct.unpack_from("<I", shader, offset + 8)[0] == (stage << 16) | 0x50,
                    "Original shader stage and model")
    return {"case": folder.name, "shader_sha256": hashlib.sha256(shader).hexdigest(),
            "precision": expected, "adapter": report["adapter"], "control": not any(expected)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    require(not args.out.exists(), "Use a new verification path")
    expected = {"-".join(map(str, case)) for case in itertools.product(
        (0, 4318, 32902), (0, 1, 5), (0, 1), (0, 1), (1, 2), (1, 2, 3, 4))}
    require({p.name for p in args.root.iterdir() if p.is_dir()} == expected and
            {p.name for p in args.baseline.iterdir() if p.is_dir()} == expected, "Complete 288-case matrices")
    cases = [verify(args.root / name, args.baseline) for name in sorted(expected)]
    signatures = {}
    rejected = []
    for case in cases:
        key = case["case"].split("-", 1)[1]
        require(signatures.setdefault(key, case["shader_sha256"]) == case["shader_sha256"],
                "Adapter-independent original bytecode")
        folder = args.root / case["case"]
        original = json.loads((folder / "report.json").read_text("utf-8"))
        for corruption in ("bits", "cache", "validity", "adapter", "precision", "opcode"):
            changed = copy.deepcopy(original)
            if corruption == "bits":
                changed["native"][0][0] ^= 1
            elif corruption == "cache":
                changed["reversed"] = changed["native"]
            elif corruption == "validity":
                changed["unknown_validity"][0] = 15
            elif corruption == "adapter":
                changed["adapter"]["vendor_id"] = -1
            elif corruption == "precision":
                changed["precision"][0] ^= 1
            else:
                changed["opcode"] = 0
            try:
                verify(folder, args.baseline, changed)
            except ValueError:
                rejected.append(case["case"] + ":" + corruption)
            else:
                raise ValueError("Accepted corruption: " + corruption)
    result = {"schema": 1, "ok": True, "complete_release_acceptance": False, "cases": cases,
              "controls": sum(case["control"] for case in cases), "original_vectors": 1152,
              "native_vectors": 2304, "unknown_results": 576, "rejected_controls": rejected}
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"cases": len(cases), "controls": result["controls"], "rejected": len(rejected), "ok": True}))


if __name__ == "__main__":
    main()
