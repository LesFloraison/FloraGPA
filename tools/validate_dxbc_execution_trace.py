"""Independently replay and rewind a version-one native execution journal.

This validates serialization and state continuity, not DXBC instruction semantics
or invocation eligibility. The companion probe report supplies storage digests
observed independently of the journal serializer.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import tempfile


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def integer(value, maximum=(1 << 64) - 1):
    require(type(value) is int and 0 <= value <= maximum, "Invalid unsigned trace integer")
    return value


def register(value):
    require(set(value) == {"dxbc_operand_kind", "indices"}, "Invalid register identity")
    return integer(value["dxbc_operand_kind"], 255), tuple(integer(v, (1 << 32) - 1) for v in value["indices"])


def value(raw):
    require(set(raw) == {"bits", "valid_components"} and len(raw["bits"]) == 4, "Invalid register value")
    return tuple(integer(v, (1 << 32) - 1) for v in raw["bits"]), integer(raw["valid_components"], 15)


def binary(text, validity=False):
    require(type(text) is str and len(text) % 2 == 0, "Invalid trace hex string")
    result = bytes.fromhex(text)
    require(result.hex() == text, "Noncanonical trace hex string")
    require(not validity or all(v in (0, 1) for v in result), "Invalid storage validity")
    return result


def counter(raw):
    require(set(raw) == {"kind", "value"} and raw["kind"] in ("ordered", "append"), "Invalid counter")
    if raw["value"] is not None:
        integer(raw["value"], (1 << 32) - 1)
    return raw["kind"], raw["value"]


def validate(path, report, reader_report=None):
    path = Path(path)
    require(path.stat().st_size <= 256 * 1024 * 1024, "Validator trace size limit exceeded")
    storage, initialized, registers, counters, instructions = {}, {}, {}, {}, {}
    initial_registers, initial_counters, initial_hashes = {}, {}, {}
    offsets, lanes = [], {}
    header = complete = None
    records = 0
    selected_steps = 0
    texture_writes = texture_bytes = 0
    changed_registers = set()
    memory_total = 0

    def canonical_hash(raw):
        return digest(json.dumps(raw, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8"))

    def projected_state():
        register_rows = [{"lane": lane, "dxbc_operand_kind": kind, "indices": list(indices),
                          "bits": list(bits), "valid_components": valid}
                         for (lane, (kind, indices)), (bits, valid) in sorted(registers.items())]
        spaces = {"buffer": 0, "shared": 1, "texture": 2}
        storage_rows = [{"space": space, "resource": resource, "bytes": len(data),
                         "sha256": digest(data), "validity_sha256": digest(valid)}
                        for (space, resource), (data, valid) in sorted(
                            storage.items(), key=lambda item: (spaces[item[0][0]], item[0][1]))]
        counter_rows = [{"view": view, "kind": kind, "value": count}
                        for view, (kind, count) in sorted(counters.items())]
        return {"registers_sha256": canonical_hash(register_rows), "registers": len(register_rows),
                "storage": storage_rows, "counters": counter_rows}

    if reader_report is not None:
        require(reader_report["format"] == "flora.dxbc-trace-reader-probe" and reader_report["version"] == 1,
                "Unexpected C++ reader report format")
        require(reader_report["trace_sha256"] == digest(path.read_bytes()), "C++ reader trace digest mismatch")
        require(reader_report["round_trip"] is True, "C++ reader did not confirm exact restoration")

    def storage_hashes():
        return {key: (digest(data), digest(valid)) for key, (data, valid) in storage.items()}

    def apply(row, forward):
        before, after = ("before", "after") if forward else ("after", "before")
        def ordered(items):
            return items if forward else reversed(items)
        for change in ordered(row["register_changes"]):
            key = row["lane"], register(change["register"])
            prior, following = value(change[before]), value(change[after])
            require(registers.get(key, ((0, 0, 0, 0), 0)) == prior, "Register delta chain mismatch")
            registers[key] = following
            changed_registers.add(key)
        for change in ordered(row["counter_changes"]):
            key = integer(change["view"])
            require(counters[key] == counter(change[before]), "Counter delta chain mismatch")
            counters[key] = counter(change[after])
        for space in ("buffer", "shared", "texture"):
            for change in ordered(row[space + "_changes"]):
                key = space, integer(change["resource"])
                data, valid = storage[key]
                first = integer(change["offset"])
                prior = binary(change[before + "_hex"])
                following = binary(change[after + "_hex"])
                prior_valid = binary(change[before + "_valid_hex"], True)
                next_valid = binary(change[after + "_valid_hex"], True)
                require(len(prior) == len(following) == len(prior_valid) == len(next_valid), "Delta size mismatch")
                last = first + len(prior)
                require(last <= len(data), "Delta outside initial storage")
                require(data[first:last] == prior and valid[first:last] == prior_valid, "Storage delta chain mismatch")
                data[first:last], valid[first:last] = following, next_valid

    with path.open("rb") as file:
        while True:
            offset = file.tell()
            line = file.readline(256 * 1024 * 1024 + 1)
            if not line:
                break
            require(line.endswith(b"\n") and len(line) <= 256 * 1024 * 1024, "Truncated or oversized trace record")
            require(complete is None, "Records after completion")
            row = json.loads(line)
            kind = row["record"]
            records += 1
            if header is None:
                require(kind == "header", "Missing trace header")
                header = row
                require(row["format"] == "flora.dxbc-execution-trace" and row["version"] == 1, "Unsupported trace schema")
                pixel = row["stage"] == "ps"
                require((pixel and row["scope"] == "one_pixel_quad") or
                        (row["stage"] == "cs" and row["scope"] == "one_cooperative_group"), "Unexpected trace scope")
                require(row["captured_gpu_schedule"] is False, "Invalid captured schedule claim")
                require(row["invocation_eligibility"] == "not_established_by_trace_writer", "Unsupported eligibility claim")
                for key, probe_key in (("frame_sha256", "frame_sha256"), ("shader_sha256", "shader_sha256"),
                                       ("gpa_event", "event"), ("gpa_shader", "shader")):
                    require(row[key] == report[probe_key], "Trace provenance mismatch: " + key)
                require(row["adapter"]["luid"] == report["adapter_luid"], "Trace adapter mismatch")
                count = 4 if pixel else 1
                if pixel:
                    require(row["execution_model"] in ("coherent-pixel-sampling/2", "coherent-pixel-memory/3"),
                            "Unknown pixel execution model")
                    active = integer(row["initial_active_mask"], 15)
                else:
                    for axis in row["group_size"]:
                        count *= integer(axis, 1024)
                require(0 < count <= 1024 and row["selected_lane"] < count, "Invalid lane count")
                continue
            if kind == "step":
                if not offsets:
                    require(all(initialized[k] == len(v[0]) for k, v in storage.items()), "Incomplete initial storage")
                    initial_hashes = storage_hashes()
                    initial_registers, initial_counters = registers.copy(), counters.copy()
                    if reader_report is not None:
                        require(reader_report["initial"] == projected_state(), "C++ reader initial state mismatch")
                    for space, items in (("buffer", report["buffers"]), ("texture", report["textures"])):
                        for item in items:
                            require(initial_hashes[(space, item["resource"])] ==
                                    (item["sha256"], item["validity_sha256"]), "Initial storage digest mismatch")
                require(row["sequence"] == len(offsets), "Noncontiguous group sequence")
                lane = integer(row["lane"], count - 1)
                require(row["lane_sequence"] == lanes.get(lane, 0), "Noncontiguous lane sequence")
                lanes[lane] = row["lane_sequence"] + 1
                inst = instructions[row["instruction"]]
                require(inst["word_offset"] == row["word_offset"] and not inst["declaration"], "Instruction identity mismatch")
                if pixel:
                    helper = not bool(active & (1 << lane))
                    require(row["helper_before"] is helper, "Pixel helper chain mismatch")
                    if row["discarded"]:
                        require(inst["dxbc_opcode"] == 13, "Discard on a different opcode")
                        active &= ~(1 << lane)
                    require(row["active_mask_after"] == active, "Pixel active-mask chain mismatch")
                    sync = 0x4000 if inst["dxbc_opcode"] == 190 and header["execution_model"] == "coherent-pixel-memory/3" else 0
                    require(row["barrier_epoch"] == 0 and row["released_barrier"] is False
                            and row["sync_flags"] == sync and not row["shared_changes"], "Compute state in pixel trace")
                    require(not helper or not any(row[k] for k in ("buffer_changes", "texture_changes", "counter_changes")),
                            "Pixel helper has side effects")
                if row["next_instruction"] is not None:
                    require(row["next_instruction"] in instructions, "Unknown next instruction")
                require(all(v in instructions for v in row["return_instruction_stack"]), "Unknown return instruction")
                if reader_report is not None and lane == header["selected_lane"]:
                    boundary = reader_report["selected_boundaries"][selected_steps]
                    require(boundary == {"position": len(offsets), "instruction": row["instruction"],
                                         "state_sha256": canonical_hash(projected_state())},
                            "C++ reader selected boundary state mismatch")
                selected_steps += lane == header["selected_lane"]
                texture_writes += len(row["texture_changes"])
                texture_bytes += sum(len(binary(c["after_hex"])) for c in row["texture_changes"])
                apply(row, True)
                offsets.append(offset)
            elif kind == "complete":
                complete = row
                require(row["completed"] is True and row["steps"] == len(offsets), "Incomplete execution")
                require(row["selected_steps"] == selected_steps, "Selected lane count mismatch")
                require(len(offsets) <= header["max_steps"], "Step budget exceeded")
                if pixel:
                    require(row["final_active_mask"] == active, "Pixel terminal mask differs")
            else:
                require(not offsets, "Input record after execution started")
                if kind == "instruction":
                    require(row["index"] == len(instructions), "Noncontiguous instruction table")
                    instructions[row["index"]] = row
                elif kind == "register":
                    key = integer(row["lane"], count - 1), register(row["register"])
                    require(key not in registers, "Duplicate initial register")
                    registers[key] = value(row["value"])
                elif kind == "counter":
                    key = integer(row["view"])
                    require(key not in counters, "Duplicate initial counter")
                    counters[key] = counter(row["value"])
                elif kind == "storage":
                    key = row["space"], integer(row["resource"])
                    size = integer(row["bytes"], 256 * 1024 * 1024)
                    memory_total += size
                    require(memory_total <= 256 * 1024 * 1024 and key not in storage, "Initial storage limit or identity conflict")
                    storage[key], initialized[key] = (bytearray(size), bytearray(size)), 0
                elif kind == "storage_bytes":
                    key = row["space"], integer(row["resource"])
                    data, valid = storage[key]
                    raw, mask = binary(row["bits_hex"]), binary(row["valid_hex"], True)
                    first = integer(row["offset"])
                    last = first + len(raw)
                    require(first == initialized[key] and len(raw) == len(mask) and last <= len(data), "Initial chunk overlap or gap")
                    data[first:last], valid[first:last] = raw, mask
                    initialized[key] = last
                else:
                    require(kind in {"source", "binding", "unavailable_binding", "unavailable_counter", "constant",
                                     "texture_metadata", "sampler", "sample_queries", "texture_view",
                                     "multisample_observation", "multisample_values", "texture_subresource"}, "Unknown trace record")
        require(complete is not None and offsets, "Missing execution completion")
        if reader_report is not None:
            require(reader_report["header"] == header and reader_report["completion"] == complete,
                    "C++ reader metadata mismatch")
            require(len(reader_report["selected_boundaries"]) == selected_steps,
                    "C++ reader selected boundary count mismatch")
            require(reader_report["final"] == projected_state(), "C++ reader final state mismatch")
        require(file.tell() == report["trace"]["bytes"] <= header["max_bytes"], "Trace byte count mismatch")
        require(records == report["trace"]["records"], "Trace record count mismatch")
        for key in ("steps", "selected_steps", "gpu_operations", "sampling_batches", "gpu"):
            require(complete[key] == report["trace"][key], "Completion summary mismatch: " + key)
        final_hashes = storage_hashes()
        for space, items in (("buffer", report["trace"]["buffers_after"]), ("texture", report["trace"]["textures_after"])):
            for item in items:
                require(final_hashes[(space, item["resource"])] ==
                        (item["sha256"], item["validity_sha256"]), "Final storage digest mismatch")
        for offset in reversed(offsets):
            file.seek(offset)
            apply(json.loads(file.readline()), False)
        require(storage_hashes() == initial_hashes, "Rewind storage mismatch")
        require(counters == initial_counters, "Rewind counter mismatch")
        for key in changed_registers | initial_registers.keys():
            require(registers[key] == initial_registers.get(key, ((0, 0, 0, 0), 0)), "Rewind register mismatch")
    return {"format": "flora.execution-trace-validation", "version": 1, "passed": True,
            "scope": "serialization_and_reversible_state_continuity", "complete_release_acceptance": False,
            "trace_sha256": digest(path.read_bytes()), "trace_bytes": path.stat().st_size,
            "header": header, "steps": len(offsets), "selected_steps": selected_steps,
            "texture_writes": texture_writes, "texture_write_bytes": texture_bytes,
            "storage_bytes": memory_total, "rewound": True}


def negative_checks(path, report):
    original = Path(path).read_bytes().splitlines(keepends=True)
    step_index = next(i for i, line in enumerate(original)
                      if json.loads(line)["record"] == "step" and json.loads(line)["register_changes"])
    chunk_index = next(i for i, line in enumerate(original) if json.loads(line)["record"] == "storage_bytes")
    results = []
    with tempfile.TemporaryDirectory(prefix="flora-trace-negative-") as temporary:
        candidate = Path(temporary) / "trace.jsonl"
        for name in ("version", "shader_identity", "sequence", "register_before", "invalid_mask",
                     "missing_chunk", "missing_completion", "truncated_record", "final_digest", "completion_steps"):
            rows = original.copy()
            evidence = copy.deepcopy(report)
            if name in ("version", "shader_identity"):
                row = json.loads(rows[0])
                row["version" if name == "version" else "shader_sha256"] = 99 if name == "version" else "0" * 64
                rows[0] = (json.dumps(row) + "\n").encode()
            elif name in ("sequence", "register_before", "invalid_mask"):
                row = json.loads(rows[step_index])
                if name == "sequence":
                    row["sequence"] += 1
                elif name == "register_before":
                    row["register_changes"][0]["before"]["bits"][0] ^= 1
                else:
                    row["register_changes"][0]["after"]["valid_components"] = 16
                rows[step_index] = (json.dumps(row) + "\n").encode()
            elif name == "missing_chunk":
                del rows[chunk_index]
            elif name == "missing_completion":
                rows.pop()
            elif name == "truncated_record":
                rows[-1] = rows[-1][:-1]
            elif name == "final_digest":
                items = evidence["trace"]["textures_after"] or evidence["trace"]["buffers_after"]
                items[0]["sha256"] = "0" * 64
            elif name == "completion_steps":
                row = json.loads(rows[-1])
                row["steps"] += 1
                rows[-1] = (json.dumps(row) + "\n").encode()
            raw = b"".join(rows)
            # Keep transport counts consistent to exercise the semantic check.
            evidence["trace"]["bytes"], evidence["trace"]["records"] = len(raw), len(rows)
            candidate.write_bytes(raw)
            try:
                validate(candidate, evidence)
            except (ValueError, KeyError, IndexError) as error:
                results.append({"case": name, "rejected": True, "reason": str(error)})
            else:
                raise ValueError("Negative trace accepted: " + name)
    return {"passed": True, "cases": results}


def reader_negative_checks(path, report, reader_report):
    results = []
    for name in ("format", "digest", "round_trip", "initial_registers", "final_validity",
                 "selected_state", "selected_position", "missing_selected", "header", "completion"):
        candidate = copy.deepcopy(reader_report)
        if name == "format":
            candidate["format"] = "unknown"
        elif name == "digest":
            candidate["trace_sha256"] = "0" * 64
        elif name == "round_trip":
            candidate["round_trip"] = False
        elif name == "initial_registers":
            candidate["initial"]["registers_sha256"] = "0" * 64
        elif name == "final_validity":
            candidate["final"]["storage"][0]["validity_sha256"] = "0" * 64
        elif name == "selected_state":
            candidate["selected_boundaries"][0]["state_sha256"] = "0" * 64
        elif name == "selected_position":
            candidate["selected_boundaries"][0]["position"] += 1
        elif name == "missing_selected":
            candidate["selected_boundaries"].pop()
        elif name == "header":
            candidate["header"]["selected_lane"] += 1
        elif name == "completion":
            candidate["completion"]["steps"] += 1
        try:
            validate(path, report, candidate)
        except (ValueError, KeyError, IndexError) as error:
            results.append({"case": name, "rejected": True, "reason": str(error)})
        else:
            raise ValueError("Corrupt C++ reader report accepted: " + name)
    return {"passed": True, "cases": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--negative-out", type=Path)
    parser.add_argument("--reader-report", type=Path,
                        help="Also compare the C++ reader's exact initial, final and selected boundary states")
    parser.add_argument("--reader-negative-out", type=Path)
    args = parser.parse_args()
    if args.reader_negative_out and not args.reader_report:
        parser.error("--reader-negative-out requires --reader-report")
    report = json.loads(args.report.read_text(encoding="utf-8"))
    reader_report = json.loads(args.reader_report.read_text(encoding="utf-8")) if args.reader_report else None
    result = validate(args.trace, report, reader_report)
    if args.reader_report:
        result["reader_report_sha256"] = digest(args.reader_report.read_bytes())
        result["reader_states_match"] = True
    result["report_sha256"] = digest(args.report.read_bytes())
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if args.negative_out:
        args.negative_out.write_text(json.dumps(negative_checks(args.trace, report), indent=2) + "\n", encoding="utf-8")
    if args.reader_negative_out:
        args.reader_negative_out.write_text(
            json.dumps(reader_negative_checks(args.trace, report, reader_report), indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": result["passed"], "steps": result["steps"], "rewound": result["rewound"]}))


if __name__ == "__main__":
    main()
