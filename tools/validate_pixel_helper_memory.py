"""Check independent pixel-memory observations, masks and hidden counter constraints."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path
import struct

from validate_typed_load_precision import require, shader_rows


def verify(report, shader):
    require(report["scope"] == "pixel_helper_memory" and report["output_scope"] == "active_pixels_only",
            "Wrong observation scope")
    require(report["model"] == "coherent-pixel-memory/3", "Wrong execution model")
    coverage, discarded, operation = (report[k] for k in ("coverage", "discarded", "operation"))
    active = coverage & ~discarded
    require(report["active_mask"] == active, "Wrong active mask")
    require(coverage in (15, 3, 5, 1) and discarded in (0, 1, 2, 5, 15), "Unexpected selection")
    require(operation in range(6) and (operation < 4 or report["structured"]), "Invalid operation")
    gpu = report["gpu"]
    require(gpu["backend"] == "dxbc-gpu/20" and gpu["warp"] == (report["vendor"] == 0) and
            gpu["vendor_id"] == (report["vendor"] or 5140), "Wrong GPU identity")
    require(all(gpu[k] == v for k, v in report["reference_adapter"].items()), "Reference adapter differs")
    stage, rows = shader_rows(shader)
    opcodes = {row[1][0] & 2047 for row in rows}
    require(stage == 0 and {13, 123, 125, 167 if report["structured"] else 165,
                           168 if report["structured"] else 166,
                           162 if report["structured"] else 161} <= opcodes,
            "Shader does not contain the declared pixel-memory fixture")
    if operation:
        require({1: 180, 2: 184, 3: 185, 4: 178, 5: 179}[operation] in opcodes,
                "Missing declared atomic operation")
    if operation >= 4:
        require(any(r[1][0] & 2047 == 158 and r[1][0] & 0x800000 for r in rows),
                "Missing ordered counter declaration")
    words = [7, 8, 9, 10, 50, 51, 52, 53]
    for i in range(4):
        if coverage & (1 << i):
            words[i] = 101 + i
        if active & (1 << i) and operation < 4:
            words[4 + i] = 53 + i if operation < 2 else 42
    require(report["expected_words"] == words and report["actual_bytes"] == list(struct.pack("<8I", *words)),
            "Memory writes disagree with coverage/discard semantics")
    count = 8 + (1 if operation == 4 else -1) * (coverage.bit_count() + active.bit_count())
    expected_counter = count if operation >= 4 else None
    require(report["expected_counter"] == report["actual_counter"] == expected_counter,
            "Hidden counter changed outside active invocation accesses")
    components = 0
    for name in ("expected_pixels", "actual_pixels"):
        pixels = report[name]
        require(len(pixels) == 4 and all(len(p) == 4 for p in pixels), "Incomplete output")
        seen = set()
        for i, pixel in enumerate(pixels):
            if not active & (1 << i):
                if name == "expected_pixels":
                    require(pixel == [0] * 4, "Inactive render target was written")
                continue
            gradients = [struct.unpack("<I", struct.pack("<f", v))[0]
                         for v in ((2, 2, 12, 12)[i], (7, 17, 7, 17)[i])]
            require(pixel[:2] == gradients, "Helper SRV derivatives differ")
            if operation < 4:
                require(pixel[2:] == [50 + i, words[4 + i]], "Active UAV results differ")
            else:
                require((pixel[2] < pixel[3]) if operation == 4 else (pixel[2] > pixel[3]),
                        "Counter violates invocation order")
                for value in pixel[2:]:
                    require((8 <= value < count) if operation == 4 else (count <= value < 8),
                            "Counter result outside legal range")
                    require(value not in seen, "Counter allocation duplicated")
                    seen.add(value)
            if name == "actual_pixels":
                components += 4 if operation < 4 else 2
    if coverage != 15 or discarded:
        require(report["helper_memory_steps"] > 0, "Missing helper memory execution")
        if operation < 4 or coverage != 15:
            require(report["helper_srv_reads"] > 0, "Missing known helper SRV read")
    return components


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    seen, selected, controls, artifacts, components = set(), set(), {}, [], 0
    for path in sorted(args.directory.glob("*.json")):
        report = json.loads(path.read_text())
        shader = path.with_suffix(".dxbc").read_bytes()
        key = tuple(report[k] for k in ("vendor", "structured", "coverage", "discarded", "operation"))
        require(key not in seen, "Duplicate observation")
        seen.add(key)
        components += verify(report, shader)
        for artifact in (path, path.with_suffix(".dxbc")):
            data = artifact.read_bytes()
            artifacts.append(dict(path=str(artifact), bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
        representative = key[:2] + key[-1:]
        if representative in selected or not report["active_mask"]:
            continue
        selected.add(representative)
        for name in ("bits", "storage", "adapter", "scope", "model", "mask", "counter", "structure", "operation"):
            changed = copy.deepcopy(report)
            if name == "bits":
                lane = next(i for i in range(4) if report["active_mask"] & (1 << i))
                changed["actual_pixels"][lane][0] ^= 1
            elif name == "storage": changed["actual_bytes"][0] ^= 1
            elif name == "adapter": changed["reference_adapter"]["luid_low"] ^= 1
            elif name == "scope": changed["output_scope"] = "all_helpers_known"
            elif name == "model": changed["model"] = "unknown"
            elif name == "mask": changed["active_mask"] ^= 1
            elif name == "counter": changed["actual_counter"] = (changed["actual_counter"] or 0) + 1
            elif name == "structure": changed["structured"] = not changed["structured"]
            else: changed["operation"] = (changed["operation"] + 1) % 4
            try:
                verify(changed, shader)
            except ValueError as error:
                controls[str(representative) + name] = str(error)
            else:
                raise ValueError("Altered evidence accepted: " + name)
    expected = set()
    for vendor, structured, coverage, discarded in itertools.product((0, 4318, 32902), (False, True),
                                                                    (15, 3, 5, 1), (0, 1, 2, 5, 15)):
        for operation in range(6 if structured else 4):
            expected.add((vendor, structured, coverage, discarded, operation))
    require(seen == expected and len(selected) == 30, "Incomplete pixel memory matrix")
    result = dict(schema=1, scope="active_output_and_storage_semantics_not_undefined_helper_bits",
                  cases=len(seen), exact_output_components=components, exact_storage_words=8 * len(seen),
                  counter_cases=120, negative_controls=controls, artifacts=artifacts)
    with args.out.open("x", encoding="utf-8") as out:
        json.dump(result, out, indent=2)
        out.write("\n")
    print(json.dumps({k: v for k, v in result.items() if k not in ("artifacts", "negative_controls")}, indent=2))


if __name__ == "__main__":
    main()
