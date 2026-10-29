"""Validate minimum-precision PS clamp evidence and its unchanged GPU references."""
import argparse
import copy
import hashlib
import json
from pathlib import Path

from validate_native_clamp_feedback import operand_end
from validate_feedback_relocation import instructions, require


def verify(folder, baseline, report=None):
    vendor, mode, precision = map(int, folder.name.split("-"))
    report = report if report is not None else json.loads((folder / "report.json").read_text("utf-8"))
    require((report["mode"], report["precision"]) == (mode, precision), "Case identity")
    require(report["adapter"]["vendor_id"] == (vendor or 5140) and
            report["adapter"]["warp"] is (vendor == 0) and
            report["adapter"]["feature_level"] == 0xb100, "Explicit adapter and feature level")
    shader = (folder / "shader.dxbc").read_bytes()
    require(shader == (baseline / folder.name / "shader.dxbc").read_bytes(), "Unchanged original shader")
    original = json.loads((folder / "original.json").read_text("utf-8"))
    require(original == json.loads((baseline / folder.name / "original.json").read_text("utf-8")),
            "Unchanged independently executed GPU result")
    require(original == report["original"] == report["native"] and len(original) == 4 and
            all(len(row) == 4 and all(type(v) is int and 0 <= v <= 0xffffffff for v in row) for row in original),
            "Exact raw result vectors")
    require(len({tuple(row) for row in original}) >= 2, "Clamp changes the original GPU result")
    require(report["reversed_clamps"] == list(reversed(original)), "Cached helper consumes changed clamps")
    require(report["unknown_clamp_validity"] == [15, 0, 15, 15], "Unknown clamp validity propagation")
    rows = instructions(folder / "shader.dxbc")
    samples = [row for row in rows if 230 <= (row[0] & 2047) <= 233]
    require(len(samples) == 1, "One clamped sampling instruction")
    row = samples[0]
    opcode = {0: 230, 1: 231, 3: 233}[mode]
    require(row[0] & 2047 == opcode, "Requested clamp opcode")
    pos = 1
    while row[pos - 1] >> 31:
        pos += 1
    precisions = []
    for _ in range(6 if mode == 0 else 7):
        start = pos
        pos = operand_end(row, pos)
        if row[start] >> 31:
            require(row[start + 1] & 63 == 1 and not row[start + 1] >> 31,
                    "Single operand modifier extension")
        precisions.append((row[start + 1] >> 14) & 7 if row[start] >> 31 else 0)
    require(set(precisions) <= {0, precision} and any(precisions), "Actual minimum-precision operands")
    require(all(precisions[i] == 0 for i in (1, 3, 4, len(precisions) - 1)),
            "Full-precision status, bindings and clamp")
    require(pos == len(row), "Complete instruction framing")
    return {"case": folder.name, "opcode": opcode, "operand_precision": precisions,
            "shader_sha256": hashlib.sha256(shader).hexdigest(), "adapter": report["adapter"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    require(not args.out.exists(), "Use a new report path")
    expected = {f"{v}-{m}-{p}" for v in (0, 4318, 32902) for m in (0, 1, 3) for p in (1, 2)}
    require({p.name for p in args.root.iterdir() if p.is_dir()} == expected and
            {p.name for p in args.baseline.iterdir() if p.is_dir()} == expected, "Complete 18-case matrices")
    cases = [verify(args.root / name, args.baseline) for name in sorted(expected)]
    signatures = {}
    for case in cases:
        _, mode, precision = case["case"].split("-")
        require(signatures.setdefault((mode, precision), case["shader_sha256"]) == case["shader_sha256"],
                "Adapter-independent reference shader")
    controls = []
    for case in cases:
        folder = args.root / case["case"]
        original = json.loads((folder / "report.json").read_text("utf-8"))
        for mutation in ("bits", "cache", "validity", "adapter", "precision"):
            report = copy.deepcopy(original)
            if mutation == "bits":
                report["native"][0][0] ^= 1
            elif mutation == "cache":
                report["reversed_clamps"] = report["native"]
            elif mutation == "validity":
                report["unknown_clamp_validity"][1] = 15
            elif mutation == "adapter":
                report["adapter"]["vendor_id"] = -1
            else:
                report["precision"] = 0
            try:
                verify(folder, args.baseline, report)
            except (AssertionError, ValueError):
                controls.append(case["case"] + ":" + mutation)
            else:
                raise AssertionError("Accepted corruption: " + mutation)
    result = {"schema": 1, "ok": True, "complete_release_acceptance": False, "cases": cases,
              "original_vectors": 72, "native_vectors": 144, "unknown_clamp_lanes": 18,
              "rejected_controls": controls}
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"cases": len(cases), "rejected_controls": len(controls), "ok": True}))


if __name__ == "__main__":
    main()
