"""Verify explicit sampling output precision, preserved components and baseline identity."""
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
    vendor, stage, comparison, precision = map(int, folder.name.split("-"))
    report = report if report is not None else json.loads((folder / "report.json").read_text("utf-8"))
    require(report["stage"] == stage and report["comparison"] is bool(comparison) and
            report["precision"] == precision, "Case identity")
    require(report["adapter"]["vendor_id"] == (vendor or 5140) and
            report["adapter"]["warp"] is (vendor == 0) and report["adapter"]["feature_level"] == 0xb100,
            "Explicit adapter and feature level")
    original = json.loads((folder / "original.json").read_text("utf-8"))
    if precision:
        require(original == json.loads((baseline / folder.name / "original.json").read_text("utf-8")),
                "Unchanged pre-product-change GPU reference")
    expected = {f"{f}-{s}-{m}-{p}.dxbc" for f, s, m, p in itertools.product(
        (0, 1), (0, 3, 7) if stage == 0 else (0,), (15, 9, 6), (0, precision) if precision else (1, 2))}
    indexed = {r["shader"]: r for r in report["results"]}
    require(len(indexed) == len(report["results"]) == len(original) == len(expected) and
            set(indexed) == {r["shader"] for r in original} == expected, "Complete unique output matrix")
    shaders = []
    for reference in original:
        name = reference["shader"]
        feedback, slot, mask, source_precision = map(int, name[:-5].split("-"))
        require(reference["feedback"] is bool(feedback) and reference["slot"] == slot and
                reference["mask"] == mask and reference["source_precision"] == source_precision,
                "Reference metadata")
        result = indexed[name]
        require(result["native"] == reference["outputs"] and len(result["native"]) == 4 and
                all(len(v) == 4 and all(type(x) is int and 0 <= x <= 0xffffffff for x in v)
                    for v in result["native"]), "Exact output vectors")
        require(len(result["unknown"]) == 2, "Both unknown input probes")
        for value in result["unknown"]:
            require(value["valid"] == 15 ^ mask and len(value["bits"]) == 4,
                    "Unknown write preserves only unwritten validity")
            require(all(value["bits"][c] == reference["outputs"][0][c]
                        for c in range(4) if value["valid"] & (1 << c)), "Preserved output components")
        data = (folder / name).read_bytes()
        if precision:
            require(data == (baseline / folder.name / name).read_bytes(), "Unchanged baseline probe bytecode")
        rows = instructions(folder / name)
        count = struct.unpack_from("<I", data, 28)[0]
        for offset in struct.unpack_from("<" + "I" * count, data, 32):
            if data[offset:offset + 4] in (b"SHEX", b"SHDR"):
                require(struct.unpack_from("<I", data, offset + 8)[0] == (stage << 16) | 0x50,
                        "Original shader stage and model")
        opcode = (229 if comparison else 228) if feedback else (71 if comparison else 72)
        require([r[0] & 2047 for r in rows] == [106, 89, 88, 90, 101, 104, 54, opcode, 62],
                "Exact output probe instruction sequence")
        annotate = lambda words, p: [words[0] | 0x80000000, 1 | (p << 14), *words[1:]] if p else words
        require(rows[4][1:] == annotate([0x1020f2, slot], precision), "Output declaration")
        operation, pos, operands = rows[7], 2, []
        require(operation[1] == 1 | (15 << 9) | (1 << 13), "Immediate texel offset")
        for _ in range(5 + feedback):
            end = operand_end(operation, pos)
            operands.append(operation[pos:end])
            pos = end
        require(pos == len(operation), "Complete sampling operands")
        require(operands[0] == annotate([0x102002 | (mask << 4), slot], precision), "Sampling output operand")
        if feedback:
            require(operands[1] == [0x100012, 0], "Opaque temporary status destination")
        require(operands[1 + feedback] == annotate([0x208e46, 2, 0], source_precision) and
                operands[2 + feedback] == [0x107006 | ((0 if comparison else 0x1b) << 4), 3] and
                operands[3 + feedback] == [0x106000, 4] and
                operands[4 + feedback] == annotate([0x20800a, 2, 1], source_precision),
                "Exact bindings, swizzle and source precision")
        shaders.append({"name": name, "sha256": hashlib.sha256(data).hexdigest()})
    return {"case": folder.name, "adapter": report["adapter"], "shaders": shaders,
            "pre_product_baseline": bool(precision),
            "original_vectors": len(original) * 4, "unknown_results": len(original) * 2}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    require(not args.out.exists(), "Use a new report path")
    expected = {"-".join(map(str, c)) for c in itertools.product((0, 4318, 32902), (0, 1), (0, 1), (0, 1, 2))}
    require({p.name for p in args.root.iterdir() if p.is_dir()} == expected and
            {p.name for p in args.baseline.iterdir() if p.is_dir()} == {name for name in expected if not name.endswith("-0")},
            "Complete 36-case matrix and unchanged 24-case baseline")
    cases, rejected, signatures = [], [], {}
    for name in sorted(expected):
        folder = args.root / name
        cases.append(verify(folder, args.baseline))
        for shader in cases[-1]["shaders"]:
            key = (name.split("-", 1)[1], shader["name"])
            require(signatures.setdefault(key, shader["sha256"]) == shader["sha256"],
                    "Adapter-independent original program")
        original = json.loads((folder / "report.json").read_text("utf-8"))
        for kind in ("bits", "validity", "preserved", "adapter", "precision", "missing"):
            report = copy.deepcopy(original)
            if kind == "bits":
                report["results"][0]["native"][0][0] ^= 1
            elif kind == "validity":
                report["results"][0]["unknown"][0]["valid"] = 15
            elif kind == "preserved":
                next(r for r in report["results"] if r["shader"].split("-")[2] == "9")["unknown"][0]["bits"][1] ^= 1
            elif kind == "adapter":
                report["adapter"]["vendor_id"] = -1
            elif kind == "precision":
                report["precision"] ^= 1
            else:
                report["results"].pop()
            try:
                verify(folder, args.baseline, report)
            except ValueError:
                rejected.append(name + ":" + kind)
            else:
                raise ValueError("Accepted corruption: " + kind)
    result = {"schema": 1, "ok": True, "complete_release_acceptance": False,
              "cases": cases, "rejected_controls": rejected,
              "original_vectors": sum(c["original_vectors"] for c in cases),
              "unknown_results": sum(c["unknown_results"] for c in cases)}
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k not in ("cases", "rejected_controls")}))


if __name__ == "__main__":
    main()
