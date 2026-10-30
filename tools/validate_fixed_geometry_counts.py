"""Validate fixed GS emission evidence against retained DXBC and raw vertex words."""
import argparse
import copy
import hashlib
import json
import re
import struct
from pathlib import Path


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def emission_schedule(data):
    require(data[:4] == b"DXBC" and len(data) >= 32, "DXBC header")
    size, count = struct.unpack_from("<II", data, 24)
    require(size == len(data), "DXBC length")
    programs = []
    for offset in struct.unpack_from(f"<{count}I", data, 32):
        tag, size = struct.unpack_from("<4sI", data, offset)
        if tag in (b"SHDR", b"SHEX"):
            programs.append(struct.unpack_from(f"<{size // 4}I", data, offset + 8))
    require(len(programs) == 1, "single shader program")
    words = programs[0]
    require(words[0] == 0x20050 and words[1] == len(words), "GS 5.0 program")
    token, operations, controls, returns = 2, [], [], []
    while token < len(words):
        opcode = words[token] & 2047
        length = (words[token] >> 24) & 127
        require(length and token + length <= len(words), "instruction extent")
        if opcode in (9, 19, 20, 117, 118, 119):
            stream = 0
            if opcode >= 117:
                require(length == 3 and words[token + 1] == 0x110000, "stream operand")
                stream = words[token + 2]
            else:
                require(length == 1, "legacy emission extent")
            require(stream < 4, "stream range")
            operations.append(dict(token=token, opcode=opcode, stream=stream))
        if opcode in (2, 3, 4, 5, 6, 7, 8, 10, 13, 18, 21, 22, 23, 31, 44, 48, 63, 76, 120):
            controls.append(opcode)
        if opcode == 62:
            returns.append(token)
        token += length
    require(returns and returns[-1] == len(words) - 1, "final return")
    return dict(operations=operations, return_token=returns[-1]), controls, returns


def validate(name, report, shader, frame):
    vendor, mode = name.split("-", 1)
    require(report["schema"] == 1 and report["vendor"] == int(vendor) and report["mode"] == mode,
            "case identity")
    require(report["shader_sha256"] == digest(shader) and report["frame_sha256"] == digest(frame),
            "input digests")
    for field in ("storage_restored", "counter_restored", "clean_draw_equal"):
        require(report[field] is True, field)
    expected = b"".join(struct.pack("<4f", float("inf") if mode == "nonfinite" else 42 + vertex,
                                    instance, 42 if mode == "nonfinite" else 0, 1)
                        for instance in range(3) for vertex in range(3))
    require(report["words"] == list(struct.unpack("<36I", expected)), "independent position formula")
    full, geometry = report["full"], report["pixel_geometry"]
    for capture in (full, geometry):
        require(capture["vertices"] == 9 and capture["primitives"] == 3 and capture["stride"] == 16 and
                capture["sha256"] == digest(expected), "full geometry")
        require(capture["original_outputs_written"] is False and capture["rasterization_disabled"] is True,
                "private pre-raster inspection")
    identity = geometry["geometry_instances"]
    accepted = mode not in ("branch", "upstream")
    require(report["resolved"] is accepted and identity["status"] == ("resolved" if accepted else "unavailable"),
            "partition availability")
    require(identity["original_gpu_schedule"] is False and identity["original_fragment_eligibility"] is None and
            identity["geometry_sha256"] == digest(expected) and identity["instance_count"] == 3,
            "partition provenance")
    schedule, controls, returns = emission_schedule(shader)
    require(bool(controls) == (mode == "branch"), "control-flow fixture")
    require([(op["opcode"], op["stream"]) for op in schedule["operations"]] ==
            [(117, 0), (117, 0), (117, 0), (118, 0)], "three explicit vertices and one cut")
    selections = report["selections"]
    if not accepted:
        require(not selections and not identity["ranges"] and not identity["prefix_queries"] and
                identity["uav_independence"] is None and
                "Read resource aliases a written UAV" in identity["reason"], "retained alias exclusion")
        return
    require(returns == [schedule["return_token"]], "single return")
    require(identity["ranges"] == [dict(first_primitive=i, instance=i, primitive_count=1) for i in range(3)],
            "instance ranges")
    require([(q["instance_count"], q["primitives"]) for q in identity["prefix_queries"]] == [(1, 1), (2, 2)],
            "prefix boundaries")
    require(len(selections) == 3, "all instance selections")
    proofs = [identity["uav_independence"]]
    for instance, selected in enumerate(selections):
        capture = selected["report"]
        partition = capture["instance_selection"]
        require(selected["instance"] == instance and partition["instance"] == instance and
                partition["first_vertex"] == instance * 3 and partition["vertex_count"] == 3,
                "selection identity")
        require(selected["sha256"] == capture["sha256"] == digest(expected[instance * 48:(instance + 1) * 48]) and
                partition["full_capture"]["sha256"] == digest(expected), "full capture byte slices")
        require(partition["input_offsets_rewritten"] is False and partition["system_ids_rewritten"] is False,
                "preserved inputs")
        proofs.append(partition["uav_independence"])
    for proof in proofs:
        gs = proof["stages"]["gs"]
        require(proof["input_aliases_excluded"] is True and proof["write_slots"] == [1] and
                proof["stages"]["vs"]["terminal_geometry_counts"] is False, "upstream proof")
        require(gs["proved"] is True and gs["scope"] == "terminal_geometry_counts" and
                gs["terminal_geometry_counts"] is True and gs["terminal_domain_counts"] is False and
                gs["emission_schedule"] == schedule and gs["shader_sha256"] == digest(shader), "GS proof")
        for read in proof["resource_reads"]:
            require(read["stage"] == "gs" and read["count_dependency"] is False, "terminal resource read")
        if mode == "atomic":
            require(any(w["result"] == "terminal_geometry_value" for w in gs["write_instructions"]),
                    "live atomic return provenance")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    expected = {f"{vendor}-{mode}" for vendor in (0, 4318, 32902)
                for mode in ("read", "atomic", "nonfinite", "branch", "upstream")}
    paths = {p.stem: p for p in args.directory.glob("*.json")}
    require(paths.keys() == expected, "complete fixed-geometry matrix")
    log = args.log.read_text(encoding="utf-8")
    require(re.search(r"Totals: 85 passed, 0 failed, 0 skipped, 0 blacklisted,", log), "complete suite")
    controls = 0
    for name, path in sorted(paths.items()):
        report = json.loads(path.read_text(encoding="utf-8"))
        shader, frame = path.with_suffix(".dxbc").read_bytes(), path.with_suffix(".gpa_frame").read_bytes()
        validate(name, report, shader, frame)
        mutations = [lambda r: r.update(storage_restored=False), lambda r: r.update(shader_sha256="0" * 64),
                     lambda r: r["words"].__setitem__(0, r["words"][0] ^ 1),
                     lambda r: r.update(resolved=not r["resolved"]),
                     lambda r: r["pixel_geometry"]["geometry_instances"].update(original_gpu_schedule=True)]
        if report["resolved"]:
            mutations += [lambda r: r["selections"][0].update(sha256="0" * 64),
                          lambda r: r["pixel_geometry"]["geometry_instances"]["uav_independence"]["stages"]["gs"]["emission_schedule"]["operations"][0].update(token=0),
                          lambda r: r["selections"][0]["report"]["instance_selection"]["uav_independence"].update(input_aliases_excluded=False)]
        for mutate in mutations:
            changed = copy.deepcopy(report)
            mutate(changed)
            try:
                validate(name, changed, shader, frame)
            except ValueError:
                controls += 1
            else:
                raise ValueError("Accepted corruption in " + name)
    summary = dict(schema=1, status="ok", cases=15, resolved=9, rejected=6, corruption_controls=controls,
                   full_uav_instance_support=False, full_renderdoc_removal=False)
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(summary, output, indent=2)
        output.write("\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
