"""Validate retained typed-buffer precision probes and their independent D3D11 results."""
import argparse
import copy
import hashlib
import itertools
import json
import struct
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def shader_rows(data):
    require(data[:4] == b"DXBC", "Missing DXBC header")
    chunks = struct.unpack_from("<I", data, 28)[0]
    programs = []
    for offset in struct.unpack_from("<" + "I" * chunks, data, 32):
        if data[offset:offset + 4] in (b"SHEX", b"SHDR"):
            size = struct.unpack_from("<I", data, offset + 4)[0]
            programs.append(struct.unpack_from("<" + "I" * (size // 4), data, offset + 8))
    require(len(programs) == 1, "Ambiguous program")
    words = programs[0]
    require(len(words) == words[1], "Program length mismatch")
    rows, offset = [], 2
    while offset < len(words):
        size = (words[offset] >> 24) & 127
        require(size > 0 and offset + size <= len(words), "Invalid instruction length")
        rows.append((offset, words[offset:offset + size]))
        offset += size
    return words[0] >> 16, rows


def direct_output_rows(report):
    def row(opcode, *operands):
        words = [opcode] + [word for operand in operands for word in operand]
        words[0] |= len(words) << 24
        return tuple(words)

    def annotated(words, precision):
        return [words[0] | 0x80000000, 1 | precision << 14, *words[1:]] if precision else words

    precision = report["precision"]
    returns = 0x5555 if precision < 4 else 0x3333 if precision == 4 else 0x4444
    kind = 30 if report["typed_uav"] else 7
    output = lambda mask: annotated([0x102002 | mask << 4, 0], precision)
    seed = [0x4002, 0x3e000000, 0xbe800000, 0x3f000000, 0xbf400000] if precision < 4 else [0x4002, 17, 23, 31, 47]
    address = annotated([0x4001, report["index"]] if report["literal"] else [0x20800a, 0, 0], report["address_precision"])
    target = output(9 if report["partial"] else 15)
    resource = [0x100006 | kind << 12 | (0x43 if report["partial"] else 0x1b) << 4, 3]
    load = row(163, target, address, resource) if report["typed_uav"] else row(45 | 0x80000000, [0x80000042, 3 | returns << 6], target, address, resource)
    return [row(106 | 1 << 16 | int(report["refactoring_allowed"]) << 11),
            row(89, [0x208e46, 0, 1]),
            row((156 if report["typed_uav"] else 88) | 0x800, [0x1000f2 | kind << 12, 3], [returns]),
            row(101, output(15)), row(54, output(15), seed), load, row(62)]


def verify(report, shader, compiled, storage, backend="dxbc-gpu/19"):
    require(report["probe"] == "typed-load-precision", "Wrong probe")
    require(digest(shader) == report["shader_sha256"], "Changed shader")
    require(digest(compiled) == report["compiler_sha256"], "Changed compiler bytes")
    require(digest(storage) == report["storage_sha256"], "Changed storage")
    require(report["valid"] == 15, "Unknown output")
    require(len(report["cpu"]) == len(report["gpu"]) == 4, "Incomplete output")
    require(report["cpu"] == report["gpu"], "Output bits differ")
    helper = report["helper"]
    require(helper["backend"] == backend, "Wrong helper version")
    require(helper["warp"] == (report["vendor"] == 0), "Wrong adapter selection")
    require(all(helper[key] == value for key, value in report["reference_adapter"].items()),
            "Different reference adapter")
    require(helper["vendor_id"] == (report["vendor"] or 5140), "Wrong adapter vendor")
    stage, rows = shader_rows(shader)
    compiler_stage, compiler_rows = shader_rows(compiled)
    require(stage == compiler_stage == report["stage"], "Changed stage")
    output_destination = report.get("output_destination", False)
    if output_destination:
        require(report.get("program_origin") == "owned-direct-output", "Wrong direct output origin")
        require(stage in (0, 1), "Invalid direct output stage")
        require([row for _, row in rows] == direct_output_rows(report), "Changed direct output fixture")
        if report["index"] == 0:
            expected_validity = (15 if report["partial"] else 7) if report["typed_uav"] else (6 if report["partial"] else 0)
            require(report["unknown_validity"] == expected_validity, "Changed unknown output validity")
            require(report["cancellation_before_submission"] is True, "Missing cancellation control")
    else:
        require(report.get("program_origin", "compiler") == "compiler", "Wrong compiler program origin")
        require(len(rows) == len(compiler_rows), "Changed compiler instruction count")
        flags = []
        for (offset, row), (original_offset, original) in zip(rows, compiler_rows):
            require(offset == original_offset, "Changed instruction offset")
            if row[0] & 2047 == 106:
                require(len(row) == 1 and (row[0] & ~(1 << 11)) == (original[0] & ~(1 << 11)),
                        "Unexpected compiler flag changes")
                flags.append(bool(row[0] & (1 << 11)))
            else:
                require(row == original, "Unexpected compiler bytecode changes")
        require(flags == [report["refactoring_allowed"]], "Wrong refactoring flag")
    opcode = 163 if report["typed_uav"] else 45
    loads = [(offset, row) for offset, row in rows if row[0] & 2047 == opcode]
    require(len(loads) == 1, "Wrong typed load count")
    offset, row = loads[0]
    cursor, extended = 1, row[0] >> 31
    while extended:
        extended = row[cursor] >> 31
        cursor += 1
    require(row[cursor] >> 31 and ((row[cursor + 1] >> 14) & 7) == report["precision"],
            "Wrong destination precision")
    # Both fixture families use a directly indexed destination register.
    require(((row[cursor] >> 12) & 255) == (2 if output_destination else 0) and ((row[cursor] >> 20) & 3) == 1,
            "Unexpected load destination")
    cursor += 3
    address_precision = (row[cursor + 1] >> 14) & 7 if row[cursor] >> 31 else 0
    require(address_precision == report["address_precision"], "Wrong address precision")
    load_steps = [step for step in report["steps"] if step["opcode"] == opcode]
    require(len(load_steps) == 1 and load_steps[0]["word"] == offset, "Missing typed load execution")
    require(report["steps"][-1]["opcode"] == 62, "Incomplete execution")
    return 4


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--backend", default="dxbc-gpu/19")
    parser.add_argument("--pixel-uav", action="store_true",
                        help="Require the expanded matrix including PS typed UAV precision")
    parser.add_argument("--output-destinations", action="store_true",
                        help="Also require direct VS/PS output-register fixtures")
    args = parser.parse_args()
    seen, artifacts, controls = set(), [], {}
    components = 0
    representatives = set()
    for path in sorted(args.directory.glob("*.json")):
        report = json.loads(path.read_text())
        key = tuple(report[field] for field in ("vendor", "stage", "precision", "address_precision",
                                                "literal", "partial", "typed_uav", "refactoring_allowed", "index")) + (report.get("output_destination", False),)
        require(key not in seen, "Duplicate case")
        seen.add(key)
        inputs = [path.with_suffix(suffix).read_bytes() for suffix in (".dxbc", ".compiler.dxbc", ".bin")]
        components += verify(report, *inputs, args.backend)
        for artifact in (path, *(path.with_suffix(suffix) for suffix in (".dxbc", ".compiler.dxbc", ".bin"))):
            data = artifact.read_bytes()
            artifacts.append({"path": str(artifact), "bytes": len(data), "sha256": digest(data)})
        representative = key[0], key[1], key[6], key[9]
        if representative in representatives:
            continue
        representatives.add(representative)
        mutations = ("bits", "validity", "shader", "adapter", "stage", "precision", "flag", "trace")
        if args.output_destinations:
            mutations += ("destination",)
        if report.get("output_destination") and report["index"] == 0:
            mutations += ("unknown-validity", "cancellation")
        for name in mutations:
            changed = copy.deepcopy(report)
            if name == "bits":
                changed["cpu"][0] ^= 1
            elif name == "validity":
                changed["valid"] = 14
            elif name == "shader":
                changed["shader_sha256"] = "changed"
            elif name == "adapter":
                changed["reference_adapter"]["luid_low"] ^= 1
            elif name == "stage":
                changed["stage"] = 99
            elif name == "precision":
                changed["precision"] = 0
            elif name == "flag":
                changed["refactoring_allowed"] = not changed["refactoring_allowed"]
            elif name == "destination":
                changed["output_destination"] = not changed.get("output_destination", False)
            elif name == "unknown-validity":
                changed["unknown_validity"] ^= 1
            elif name == "cancellation":
                changed["cancellation_before_submission"] = False
            else:
                changed["steps"] = changed["steps"][:-1]
            try:
                verify(changed, *inputs, args.backend)
            except ValueError as error:
                controls[f"{key[0]}-{key[1]}-{int(key[6])}-{int(key[9])}-{name}"] = str(error)
            else:
                raise ValueError("Corruption accepted: " + name)
    expected = {case for case in itertools.product(
        (0, 4318, 32902), (0, 1, 5), (1, 2, 4, 5), (0, 4, 5), (False, True),
        (False, True), (False, True), (False, True), (0, 2, 3, 65536, 4294967295),
        (False, True) if args.output_destinations else (False,))
        if (not case[6] or case[1] == 5 or (args.pixel_uav and case[1] == 0))
        and (not case[9] or case[1] != 5)}
    require(seen == expected, "Incomplete or changed precision matrix")
    result = {"schema": 1, "cases": len(seen), "exact_components": components,
              "scope": "same_adapter_typed_load_precision_observations",
              "negative_controls": controls, "artifacts": artifacts}
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps({key: result[key] for key in ("cases", "exact_components")}))


if __name__ == "__main__":
    main()
