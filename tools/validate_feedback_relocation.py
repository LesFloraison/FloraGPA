"""Audit feedback relocation evidence without waiving failed GPU mapping checks."""
import argparse
import json
import struct
from pathlib import Path


def require(value, message):
    if not value:
        raise ValueError(message)


def instructions(path):
    data = path.read_bytes()
    require(data[:4] == b"DXBC" and len(data) >= 32, "DXBC header")
    count = struct.unpack_from("<I", data, 28)[0]
    require(32 + count * 4 <= len(data), "DXBC chunk table")
    code = []
    for offset in struct.unpack_from("<" + "I" * count, data, 32):
        require(offset + 8 <= len(data), "DXBC chunk bounds")
        size = struct.unpack_from("<I", data, offset + 4)[0]
        require(offset + 8 + size <= len(data), "DXBC payload bounds")
        if data[offset:offset + 4] not in (b"SHEX", b"SHDR"):
            continue
        require(not code and size % 4 == 0, "Unique aligned shader code")
        words = struct.unpack_from("<" + "I" * (size // 4), data, offset + 8)
        require(words[1] == len(words), "Shader word count")
        pos = 2
        while pos < len(words):
            length = (words[pos] >> 24) & 127
            require(length and pos + length <= len(words), "Instruction bounds")
            code.append(list(words[pos:pos + length]))
            pos += length
    require(code, "Missing shader code")
    return code


def operand(row, pos):
    token = row[pos]
    kind = (token >> 12) & 255
    start = pos
    pos += 1
    extended = token >> 31
    while extended:
        extended = row[pos] >> 31
        pos += 1
    index = pos
    dimensions = (token >> 20) & 3
    for dimension in range(dimensions):
        require(((token >> (22 + dimension * 3)) & 7) == 0,
                "Fixture uses immediate register indices")
        pos += 1
    if kind == 4:
        pos += 1 if token & 3 == 1 else 4
    require(start < pos <= len(row), "Fixture operand bounds")
    return pos, kind, index


def relocated(original, changed, mapping):
    require(len(original) == len(changed), "Instruction count preserved")
    for before, after in zip(original, changed):
        expected = before.copy()
        op = before[0] & 2047
        arity = {156: 1, 157: 1, 158: 1, 223: 4, 225: 4, 226: 4, 227: 5, 168: 4}.get(op)
        if arity:
            pos, extended = 1, before[0] >> 31
            while extended:
                extended = before[pos] >> 31
                pos += 1
            for _ in range(arity):
                pos, kind, index = operand(before, pos)
                if kind == 30:
                    expected[index] = mapping.get(before[index], before[index])
        require(after == expected, "Only UAV register indices may change")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    paths = {p.parent.name: p for p in args.root.glob("*/report.json")}
    layouts = {f"layout-{m}-{s}-{p}-{r}" for m in range(3) for s in range(2)
               for p in range(2) for r in range(2)}
    gpu = {f"gpu-{v}-{m}-{s}" for v in (0, 4318, 32902) for m in range(3) for s in range(2)}
    require(paths.keys() == layouts | gpu, "Complete 24-layout and 18-GPU matrix")
    rejected = []
    qualified = []
    for name, path in sorted(paths.items()):
        report = json.loads(path.read_text())
        folder = path.parent
        original = instructions(folder / "original.dxbc")
        for slot in (7, 63):
            relocated(original, instructions(folder / f"moved-{slot}.dxbc"), {0: slot})
        if name in layouts:
            require(report["roundtrip_tokens_equal"] and report["truncation_rejected"], "Layout assertions")
            if report["pixel"]:
                relocated(original, instructions(folder / "reserved.dxbc"), {0: 1, 1: 2})
            continue
        require(report["scope"] == "non_tiled_buffer_feedback_relocation" and
                report["input_unchanged"] and report["debug_errors"] == 0, "GPU diagnostic scope")
        require(report["adapter"]["tiled_resources_tier"] >= 2, "Feedback capability")
        mode = report["mode"]
        values = [102, 0, 0, 1] if mode == 0 else [102] * 4 if mode == 1 else [108, 109, 110, 111]
        expected = values + [1, 99, 0x0011e000, 123]
        runs = {"original": report["original"]}
        require([r["slot"] for r in report["relocated"]] == [7, 63], "Both destination slots")
        for row in report["relocated"]:
            runs[f"relocated_{row['slot']}"] = row["result"]
            runs[f"compiled_{row['slot']}"] = row["compiled_control"]
        for result in runs.values():
            require(len(result) == 8 and result[:4] == values and result[5:] == expected[5:],
                    "Exact data and literal sentinel results")
            require(type(result[4]) is int and result[4] in (0, 1), "Interpreted mapping Boolean")
        failures = {run: result for run, result in runs.items() if result != expected}
        if failures:
            rejected.append({"case": name, "failures": failures})
        else:
            qualified.append(name)
    result = {"schema": 1, "layout_cases": len(layouts), "gpu_cases": len(gpu),
              "qualified_gpu_cases": qualified, "unqualified_gpu_cases": rejected,
              "gpu_feedback_acceptance": not rejected, "renderdoc_removed": False}
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps({"layout_cases": len(layouts), "gpu_cases": len(gpu),
                      "qualified": len(qualified), "unqualified": len(rejected),
                      "gpu_feedback_acceptance": not rejected}))
    # A structurally sound report is not a successful semantic acceptance run.
    return 1 if rejected else 0


if __name__ == "__main__":
    raise SystemExit(main())
