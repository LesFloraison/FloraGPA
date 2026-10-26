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
    # The fixture's typed load destination is a directly indexed temporary.
    require(((row[cursor] >> 12) & 255) == 0 and ((row[cursor] >> 20) & 3) == 1,
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
    args = parser.parse_args()
    seen, artifacts, controls = set(), [], {}
    components = 0
    representatives = set()
    for path in sorted(args.directory.glob("*.json")):
        report = json.loads(path.read_text())
        key = tuple(report[field] for field in ("vendor", "stage", "precision", "address_precision",
                                                "literal", "partial", "typed_uav", "refactoring_allowed", "index"))
        require(key not in seen, "Duplicate case")
        seen.add(key)
        inputs = [path.with_suffix(suffix).read_bytes() for suffix in (".dxbc", ".compiler.dxbc", ".bin")]
        components += verify(report, *inputs, args.backend)
        for artifact in (path, *(path.with_suffix(suffix) for suffix in (".dxbc", ".compiler.dxbc", ".bin"))):
            data = artifact.read_bytes()
            artifacts.append({"path": str(artifact), "bytes": len(data), "sha256": digest(data)})
        representative = key[0], key[1]
        if representative in representatives:
            continue
        representatives.add(representative)
        for name in ("bits", "validity", "shader", "adapter", "stage", "precision", "flag", "trace"):
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
            else:
                changed["steps"] = changed["steps"][:-1]
            try:
                verify(changed, *inputs, args.backend)
            except ValueError as error:
                controls[f"{key[0]}-{key[1]}-{name}"] = str(error)
            else:
                raise ValueError("Corruption accepted: " + name)
    expected = {case for case in itertools.product(
        (0, 4318, 32902), (0, 1, 5), (1, 2, 4, 5), (0, 4, 5), (False, True),
        (False, True), (False, True), (False, True), (0, 2, 3, 65536, 4294967295))
        if not case[6] or case[1] == 5}
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
