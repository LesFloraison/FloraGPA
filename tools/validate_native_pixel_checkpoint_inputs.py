"""Validate checkpoint resource observations and typed atomic transactions."""
import argparse
import copy
import json
import hashlib
import pathlib
import re

VENDORS = (0, 4318, 32902)
MODES = ("counter", "append", "raw", "typed", "texture")
OPS = ("add", "and", "or", "xor", "min", "max", "exchange", "compare_equal",
       "compare_different", "store_equal", "store_different", "add_no_result")


def require(value, message):
    if not value:
        raise ValueError(message)


def words(raw):
    require(len(raw) % 4 == 0 and all(type(x) is int and 0 <= x <= 255 for x in raw), "raw storage")
    return [int.from_bytes(bytes(raw[i:i + 4]), "little") for i in range(0, len(raw), 4)]


def signed(value):
    return value if value < 0x80000000 else value - 0x100000000


def validate(name, report):
    require(report["schema"] == 1 and report["status"] == "ok" and
            report["original_storage_restored"] is True, "result status")
    if name.startswith("computeGroup-"):
        _, vendor, mode = name.split("-")
        require(report["vendor"] == int(vendor) and report["mode"] == mode and
                report["resource_observation"] == "current_native_bindings" and
                report["original_gpu_schedule"] is False, "compute context")
        require(sorted(words(report["results"])) == sorted(words(report["gpu_results"])) == [0, 1, 2, 3],
                "cooperative atomic returns")
        require(words(report["gpu_storage"]) == ([4, 0, 0, 0] if mode == "typed" else [4]),
                "cooperative atomic storage")
        require(len(report["transactions"]) == 4 and
                {r["lane"] for r in report["transactions"]} == set(range(4)), "cooperative transactions")
        for row in report["transactions"]:
            require(row["atomic"] and len(row["buffer_changes"] + row["texture_changes"]) == 1,
                    "cooperative atomic journal")
        return
    if name.startswith("textureRegions-"):
        _, vendor, dimension, format_id, mode = name.split("-")
        vendor, dimension, format_id, mode = map(int, (vendor, dimension, format_id, mode))
        require((report["vendor"], report["dimension"], report["format"], report["mode"]) ==
                (vendor, dimension, format_id, mode), "region identity")
        size = {2: 56, 3: 336, 5: 2336, 7: 168, 8: 1008}[dimension]
        nominal = {2: 36, 3: 276, 5: 2196, 7: 148, 8: 948}[dimension]
        possible = [nominal] if mode == 0 else ([None, 2260] if dimension == 5 and mode >= 2 else [None])
        offset = report["selected_offset"]
        require(offset in possible, "region footprint")
        before = bytearray([0x11] * size)
        marker, after = before.copy(), before.copy()
        if offset is not None:
            marker[offset:offset + 4] = (0x11111116).to_bytes(4, "little")
            if mode != 3:
                after[:] = marker
        require(report["before_sha256"] == hashlib.sha256(before).hexdigest() and
                report["marker_sha256"] == hashlib.sha256(marker).hexdigest() and
                report["storage_sha256"] == hashlib.sha256(after).hexdigest(), "region storage oracle")
        require(report["changes"] == int(offset is not None) and
                report["valid_mask"] == (15 if offset is not None else 14), "region validity")
        require(report["gpu_assisted"] == (dimension == 5 and mode != 0), "atomic footprint assistance")
        require(report["raw_bits"][1:] == words(report["gpu_color"])[1:] == [11, 17, 23], "region outputs")
        if offset is not None:
            require(report["raw_bits"][0] == words(report["gpu_color"])[0] == 0x11111111, "region old value")
        return
    if name.startswith("typedInstructions-"):
        _, vendor, mode, sign, op = name.split("-", 4)
        require(report["vendor"] == int(vendor) and report["mode"] == mode and
                report["signed"] == bool(int(sign)) and report["operation"] == op, "typed identity")
        initial = 0xfffffff7 if int(sign) else 0x80000007
        operand = 0xfffffffd if op in ("min", "max") else 5
        expected, returned = initial, initial
        if op in ("add", "add_no_result"):
            expected = (initial + operand) & 0xffffffff
        elif op == "and":
            expected = initial & operand
        elif op == "or":
            expected = initial | operand
        elif op == "xor":
            expected = initial ^ operand
        elif op in ("min", "max"):
            key = signed if int(sign) else int
            expected = (min if op == "min" else max)((initial, operand), key=key)
        elif op in ("exchange", "compare_equal", "store_equal"):
            expected = operand
        if op.startswith("store") or op == "add_no_result":
            returned = 123
        require((report["initial"], report["operand"], report["expected"], report["returned"]) ==
                (initial, operand, expected, returned), "CPU atomic oracle")
        require(words(report["gpu_storage"]) == ([expected, 0, 0, 0] if mode == "typed" else [expected]),
                "GPU atomic storage")
        require(words(report["gpu_color"]) == report["raw_bits"] == [returned, 11, 17, 23] and
                report["valid_mask"] == 15, "atomic output")
        transactions = report["transactions"]
        require(len(transactions) == 4 and {x["lane"] for x in transactions} == set(range(4)), "quad transactions")
        for transaction in transactions:
            require(transaction["atomic"] and not transaction["shared_changes"] and
                    not transaction["counter_changes"], "atomic transaction kind")
            changes = transaction["buffer_changes"] + transaction["texture_changes"]
            if transaction["lane"] or op in ("compare_different", "store_different"):
                require(not changes, "helper or failed comparison wrote storage")
            else:
                require(len(changes) == 1, "atomic transaction count")
                change = changes[0]
                require(change["resource"] == 10 and change["offset"] == 0 and
                        int.from_bytes(bytes.fromhex(change["before_hex"]), "little") == initial and
                        int.from_bytes(bytes.fromhex(change["after_hex"]), "little") == expected and
                        change["before_valid_hex"] == change["after_valid_hex"] == "01010101", "atomic journal")
        return
    if name.startswith("typedBounds-"):
        _, vendor, mode = name.split("-")
        require(report["vendor"] == int(vendor) and report["mode"] == mode and
                report["unbound_result_unknown"] is True and report["invalid_format_rejected"] is True,
                "boundary identity")
        require([r["address"] for r in report["cases"]] == [0, 1, 2, 0xffffffff], "boundary selections")
        for row in report["cases"]:
            inside = row["address"] < (2 if mode == "typed" else 1)
            expected = [0] * (4 if mode == "typed" else 1)
            if inside:
                expected[row["address"] + (1 if mode == "typed" else 0)] = 5
            require(row["in_bounds"] == inside and words(row["storage"]) == expected and
                    row["valid_mask"] == (15 if inside else 14) and row["raw_bits"][1:] == [11, 17, 23],
                    "boundary storage or validity")
        return
    if name.startswith("recovery-"):
        require(report["vendor"] == int(name.split("-")[1]) and report["replay_generation"] > 0,
                "recovery context")
        return
    _, vendor, mode = name.split("-")
    require(mode in MODES and report["mode"] == mode and report["reference_schedule"] ==
            "separate_original_draws_with_readback", "prefix identity")
    geometry = report["geometry"]
    require(geometry["original_fragment_eligibility"] is None and geometry["original_gpu_schedule"] is False
            and geometry["primitive_count"] == 3, "geometry scope")
    require(geometry["adapter"]["vendor"] == (int(vendor) or 5140), "prefix adapter")
    require([r["completed_primitives"] for r in report["checkpoints"]] == list(range(4)), "checkpoint sequence")
    for count, row in enumerate(report["checkpoints"]):
        output = row["outputs"]
        expected = [7 if mode == "append" else count, 11, 17, 23]
        require(output["resource_observation"] == "current_native_bindings" and
                output["frame_sha256"] == geometry["frame_sha256"] and output["event"] == 200 and
                output["shader"] == 32 and output["replay_generation"] > 0 and
                output["original_fragment_eligibility"] is None and output["captured_gpu_schedule"] is False,
                "checkpoint output context")
        require(words(row["normal_next_prefix"]) == expected and
                output["lanes"][0]["outputs"][0]["raw_bits"] == expected and
                output["lanes"][0]["outputs"][0]["valid_mask"] == 15, "checkpoint exact GPU comparison")
        if mode in ("counter", "append"):
            require(row["native_counter"] == count, "current counter")
        else:
            require(words(row["storage"])[0] == count, "current atomic storage")
    require([r["primitives"] for r in report["unordered_original_draws"]] == [1, 2, 3, 4], "unordered samples")
    for row in report["unordered_original_draws"]:
        value = words(row["color"])[0]
        require(value == 7 if mode == "append" else value < row["primitives"], "unordered atomic constraint")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=pathlib.Path)
    parser.add_argument("--log", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    expected = {f"typedInstructions-{v}-{m}-{s}-{op}" for v in VENDORS for m in ("typed", "texture")
                for s in (0, 1) for op in OPS}
    expected |= {f"computeGroup-{v}-{m}" for v in VENDORS for m in ("typed", "texture")}
    expected |= {f"textureRegions-{v}-{d}-{f}-{m}" for v in VENDORS for d in (2, 3, 5, 7, 8)
                 for f in (42, 43) for m in range(4)}
    expected |= {f"typedBounds-{v}-{m}" for v in VENDORS for m in ("typed", "texture")}
    expected |= {f"prefixes-{v}-{m}" for v in VENDORS for m in MODES}
    expected |= {f"recovery-{v}" for v in VENDORS}
    paths = {p.stem: p for p in args.directory.glob("*.json")}
    require(paths.keys() == expected, "evidence case set")
    require(re.search(r"Totals: 296 passed, 0 failed, 0 skipped, 0 blacklisted,", args.log.read_text()),
            "complete Qt result")
    controls = 0
    for name in sorted(expected):
        report = json.loads(paths[name].read_text())
        validate(name, report)
        broken = copy.deepcopy(report)
        if name.startswith("computeGroup-"):
            broken["original_gpu_schedule"] = True
        elif name.startswith("textureRegions-"):
            broken["valid_mask"] ^= 1
        elif name.startswith("typedInstructions-"):
            broken["raw_bits"][0] ^= 1
        elif name.startswith("typedBounds-"):
            broken["cases"][-1]["valid_mask"] = 15
        elif name.startswith("prefixes-"):
            broken["checkpoints"][1]["outputs"]["resource_observation"] = "before_event_replay"
        else:
            broken["original_storage_restored"] = False
        try:
            validate(name, broken)
        except ValueError:
            controls += 1
        else:
            raise ValueError("Corruption control accepted: " + name)
    summary = {"schema": 1, "status": "ok", "data_cases": len(expected), "corruption_controls": controls,
               "scope": "current_bound_inputs_and_serialized_typed_atomics", "complete_pixel_history": False}
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(summary, output, indent=2)
        output.write("\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
