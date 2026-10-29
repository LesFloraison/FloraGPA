"""Check seeded status probes without accepting a seed-dependent GPU result."""
import argparse
import copy
import json
from pathlib import Path

from validate_feedback_relocation import instructions, operand, require


def check_seed(original, seeded, opcode, seed):
    require(len(seeded) == len(original) + 1, "Exactly one status seed instruction")
    indices = [i for i, row in enumerate(original) if row[0] & 2047 == opcode]
    require(len(indices) == 1, "One feedback load in fixture")
    index = indices[0]
    row = original[index]
    pos, extended = 1, row[0] >> 31
    while extended:
        extended = row[pos] >> 31
        pos += 1
    status, _, _ = operand(row, pos)
    end, kind, _ = operand(row, status)
    require(kind == 0, "Temporary feedback destination")
    expected = [54] + row[status:end] + [0x4001, seed]
    expected[0] |= len(expected) << 24
    require(seeded[index] == expected, "Seed writes only the exact feedback destination")
    require(seeded[:index] + seeded[index + 1:] == original, "All original instructions preserved")


def outputs(report):
    return [report["original"]] + [row[key] for row in report["relocated"]
                                   for key in ("result", "compiled_control")]


def check_outputs(zero, ones, affected):
    z, o = outputs(zero), outputs(ones)
    require(len(z) == len(o) == 5, "Original and four destination executions")
    mode = zero["mode"]
    data = [102, 0, 0, 1] if mode == 0 else [102] * 4 if mode == 1 else [108, 109, 110, 111]
    for a, b in zip(z, o):
        require(a[:4] == b[:4] == data and a[5:] == b[5:] == [99, 0x0011e000, 123],
                "Seed leaves source values and sentinels unchanged")
        require(a[4] == (0 if affected else 1) and b[4] == 1, "Observed status seed relationship")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("zero", type=Path)
    parser.add_argument("ones", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    expected = {f"gpu-{v}-{m}-{s}" for v in (0, 4318, 32902) for m in range(3) for s in range(2)}
    for root in (args.zero, args.ones):
        require({p.parent.name for p in root.glob("*/report.json")} == expected, "Complete GPU matrix")
    comparisons, corruptions = 0, 0
    affected_cases = []
    names = ["original.dxbc", "moved-7.dxbc", "moved-63.dxbc", "control-7.dxbc", "control-63.dxbc"]
    for case in sorted(expected):
        base = json.loads((args.baseline / case / "report.json").read_text())
        zero = json.loads((args.zero / case / "report.json").read_text())
        ones = json.loads((args.ones / case / "report.json").read_text())
        affected = base["adapter"]["vendor"] == 4318 and not base["srv"] and base["mode"] in (1, 2)
        for report, seed, root in ((zero, 0, args.zero), (ones, 0xffffffff, args.ones)):
            require(report["adapter"] == base["adapter"] and report["mode"] == base["mode"] and
                    report["srv"] == base["srv"] and report["opcode"] == base["opcode"], "Same input and adapter")
            require(report["status_seed"] == seed and not report["status_first"] and not report["batch_bind"],
                    "Isolated status-seed experiment")
            require(report["input_unchanged"] and report["debug_errors"] == 0, "GPU storage and validation")
            for name in names:
                original = instructions(args.baseline / case / name)
                seeded = instructions(root / case / name)
                check_seed(original, seeded, report["opcode"], seed)
                comparisons += 1
                try:
                    check_seed(original, seeded, report["opcode"], seed ^ 0xffffffff)
                except ValueError:
                    corruptions += 1
                else:
                    raise ValueError("Accepted wrong status seed")
        check_outputs(zero, ones, affected)
        wrong = copy.deepcopy(zero)
        wrong["original"][4] ^= 1
        try:
            check_outputs(wrong, ones, affected)
        except ValueError:
            corruptions += 1
        else:
            raise ValueError("Accepted changed mapping result")
        if affected:
            affected_cases.append(case)
    result = {"schema": 1, "status": "evidence_valid_semantics_failed", "cases": len(expected),
              "dispatches": len(expected) * 10, "exact_seed_insertions": comparisons,
              "corruptions_rejected": corruptions, "seed_dependent_cases": affected_cases,
              "gpu_feedback_acceptance": False, "product_seed_workaround": False,
              "renderdoc_removed": False}
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
