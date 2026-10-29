"""Validate the complete texture feedback matrix and its independent data oracle."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import struct

from validate_feedback_relocation import instructions, operand, require


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    matrix = list(itertools.product((0, 4318, 32902), (0, 1, 5), range(7), (0, 1)))
    expected_names = {"-".join(map(str, row)) for row in matrix}
    require({p.name for p in args.root.iterdir() if p.is_dir()} == expected_names,
            "Incomplete texture feedback matrix")
    expected_values = ([10 / 16] * 4, [10 / 16] * 4, [1] * 4,
                       [10 / 16, 11 / 16, 7 / 16, 6 / 16],
                       [11 / 16, 12 / 16, 8 / 16, 7 / 16], [1, 1, 0, 0], [1, 1, 1, 0])
    expected_opcodes = (223, 228, 229, 219, 221, 220, 222)
    arities = (4, 6, 6, 5, 6, 6, 7)
    cases, shaders, padded = [], {}, 0
    for vendor, stage, mode, known in matrix:
        name = f"{vendor}-{stage}-{mode}-{known}"
        folder = args.root / name
        report_path = folder / "report.json"
        report = json.loads(report_path.read_text())
        require(report["stage"] == stage and report["mode"] == mode and
                report["known_non_tiled"] is bool(known), "Fixture identity")
        require(report["adapter"]["vendor_id"] == (vendor or 5140) and
                report["adapter"]["warp"] is (vendor == 0), "Actual adapter identity")
        oracle = [struct.unpack("<I", struct.pack("<f", v))[0] for v in expected_values[mode]]
        require(report["original"] == report["control"] == oracle, "Independent texel oracle")
        require(report["valid"] == (15 if known else 7), "Unknown mapping must remain unknown")
        require(all(report["native"][i] == oracle[i] for i in range(4)
                    if report["valid"] & (1 << i)), "Native valid data differs")
        require(report["opaque_changes"] > 0, "Opaque status propagation absent")
        hashes = {}
        for kind in ("feedback", "control"):
            path = folder / f"{kind}.dxbc"
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            require(shaders.setdefault((stage, mode, kind), digest) == digest,
                    "Adapter or knowledge changed shader bytes")
            hashes[kind] = digest
        code = instructions(folder / "feedback.dxbc")
        feedback = [row for row in code if (row[0] & 2047) == expected_opcodes[mode]]
        require(len(feedback) == 1 and sum((r[0] & 2047) == 234 for r in code) == 1,
                "Expected feedback operation and mapping check")
        row, pos = feedback[0], 1
        extended = row[0] >> 31
        while extended:
            extended = row[pos] >> 31
            pos += 1
        for _ in range(arities[mode]):
            pos, _, _ = operand(row, pos)
        require(row[pos:] == ([0] if mode in (1, 2) else []), "Operand framing and compiler padding")
        padded += int(mode in (1, 2))
        require(not any(219 <= (r[0] & 2047) <= 234 for r in instructions(folder / "control.dxbc")),
                "Ordinary oracle unexpectedly uses feedback")
        cases.append({"case": name, "shaders": hashes,
                      "report_sha256": hashlib.sha256(report_path.read_bytes()).hexdigest()})
    result = {"schema": 1, "status": "passed", "cases": cases, "case_count": len(cases),
              "unique_shader_count": len(set(shaders.values())), "padded_instructions": padded,
              "scope": "synthetic_non_tiled_textures_with_known_and_unknown_provenance"}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k != "cases"}))


if __name__ == "__main__":
    main()
