"""Audit clamp execution and report semantic conflicts without waiving them."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import struct

from validate_feedback_relocation import instructions, require


def operand_end(row, pos, depth=0):
    require(depth <= 8 and pos < len(row), "Operand nesting/bounds")
    token = row[pos]
    pos += 1
    ext = token >> 31
    while ext:
        require(pos < len(row), "Operand extension bounds")
        ext = row[pos] >> 31
        pos += 1
    for dimension in range((token >> 20) & 3):
        representation = (token >> (22 + 3 * dimension)) & 7
        require(representation <= 4, "Index representation")
        pos += 1 if representation in (0, 3) else 2 if representation in (1, 4) else 0
        if representation >= 2:
            pos = operand_end(row, pos, depth + 1)
    kind = (token >> 12) & 255
    if kind in (4, 5):
        require(token & 3 in (1, 2), "Literal component count")
        pos += (1 if token & 3 == 1 else 4) * (2 if kind == 5 else 1)
    require(pos <= len(row), "Operand length")
    return pos


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    modes = [(0, i) for i in range(4)] + [(1, 2), (5, 2)]
    matrix = [(v, s, m, c) for v, (s, m), c in itertools.product((0, 4318, 32902), modes, range(9))]
    require({p.name for p in args.root.iterdir() if p.is_dir()} ==
            {"-".join(map(str, case)) for case in matrix}, "Complete clamp matrix")
    cases, shaders, conflicts, dynamic_differences = [], {}, [], []
    rejected, padded, zero_pad, gradient_pad = 0, 0, 0, 0
    for vendor, stage, mode, scenario in matrix:
        name = f"{vendor}-{stage}-{mode}-{scenario}"
        folder = args.root / name
        report = json.loads((folder / "report.json").read_text())
        require((report["stage"], report["mode"], report["scenario"]) == (stage, mode, scenario), "Case identity")
        require(report["adapter"]["vendor_id"] == (vendor or 5140) and
                report["adapter"]["warp"] is (vendor == 0), "Adapter identity")
        hashes = {}
        for kind in ("feedback", "ordinary", "unpadded", "dynamic"):
            data = (folder / f"{kind}.dxbc").read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            require(shaders.setdefault((stage, mode, kind), digest) == digest,
                    "Scenario or adapter changed shader bytes")
            hashes[kind] = digest
        original, canonical = instructions(folder / "feedback.dxbc"), instructions(folder / "unpadded.dxbc")
        require(len(original) == len(canonical), "Canonical instruction count")
        count = 0
        for before, after in zip(original, canonical):
            expected = before.copy()
            if (before[0] & 2047) == 230 + mode:
                count += 1
                pos, ext = 1, before[0] >> 31
                while ext:
                    ext = before[pos] >> 31
                    pos += 1
                for _ in range((6, 7, 8, 7)[mode]):
                    pos = operand_end(before, pos)
                tail = before[pos:]
                require(tail == [0] or (mode == 2 and tail == [0x2000]), "Observed compiler trailer")
                padded += 1
                zero_pad += int(tail == [0])
                gradient_pad += int(tail == [0x2000])
                expected.pop()
                expected[0] = (expected[0] & ~(127 << 24)) | (len(expected) << 24)
            require(after == expected, "Only the observed trailer may be removed")
        require(count == 1, "One clamp instruction")
        if scenario == 8:
            require(report["adapter"]["feature_level"] == 0xB000 and report["gpu_operations"] == 0 and
                    "feature level 11.1 and tier 2" in report["rejected"], "Unsupported-device rejection")
            rejected += 1
        else:
            require(report["adapter"]["feature_level"] == 0xB100, "Explicit feature level")
            require(report["original"] == report["unpadded"], "Trailer removal changed GPU output")
            for i, value in enumerate(report["native"]):
                valid = 0 if scenario == 5 and i == 1 else 7 if scenario == 7 else 15
                require(value["valid"] == valid, "Unknown value/status propagation")
                require(all(value["bits"][c] == report["original"][i][c]
                            for c in range(4) if valid & (1 << c)), "Native/original disagreement")
            if stage == 5 and scenario != 5:
                require(report["batches"] == 1, "Compute cohort batching")
            if stage == 0 and mode != 2:
                require(report["batches"] == 1, "Pixel quad batching")
            if report["original"] != report["dynamic"]:
                dynamic_differences.append(name)
            if stage == 0 and mode == 1:
                require(len(report["native_dynamic"]) == 4, "Dynamic cache control missing")
                for i, value in enumerate(report["native_dynamic"]):
                    require(value["valid"] == report["native"][i]["valid"], "Dynamic validity")
                    require(all(value["bits"][c] == report["dynamic"][i][c]
                                for c in range(4) if value["valid"] & (1 << c)),
                            "Literal/dynamic GPU shader cache collision")
            if scenario == 0:
                expected = [0., 0., 1.] if mode == 3 else [.25, .5, 1.]
                expected = [struct.unpack("<I", struct.pack("<f", x))[0] for x in expected]
                actual = [r[0] for r in report["original"][:3]]
                if actual != expected:
                    conflicts.append({"case": name, "expected_first_three_mips": expected,
                                      "original": actual, "dynamic": [r[0] for r in report["dynamic"][:3]]})
        cases.append({"case": name, "shaders": hashes,
                      "report_sha256": hashlib.sha256((folder / "report.json").read_bytes()).hexdigest()})
    result = {"schema": 1, "status": "semantic_conflicts" if conflicts else "passed",
              "case_count": len(cases), "rejected_unsupported_devices": rejected,
              "unique_shader_count": len(set(shaders.values())),
              "padded_instructions": padded, "zero_trailers": zero_pad, "gradient_trailers": gradient_pad,
              "dynamic_bias_differences": dynamic_differences, "semantic_conflicts": conflicts,
              "cases": cases}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k != "cases"}))
    return int(bool(conflicts))


if __name__ == "__main__":
    raise SystemExit(main())
