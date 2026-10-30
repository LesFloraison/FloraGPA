"""Check native geometry instance proof and restoration evidence."""
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


def storage_hash(values):
    return hashlib.sha256(struct.pack("<4I", *values)).hexdigest()


def validate(name, result, terminal_domain=False):
    vendor, mode = name.split("-", 1)
    require(result["schema"] == 1 and result["vendor"] == int(vendor) and result["mode"] == mode, "identity")
    for key in ("storage_restored", "counter_restored", "clean_draw_equal"):
        require(result[key] is True, key)
    gs = mode.startswith("gs")
    unbound = mode == "gs-unbound"
    rejected = (mode == "read" and not terminal_domain) or "alias" in mode
    require(result["before_sha256"] == storage_hash((42, 0, 0, 0) if mode in ("ds-atomic", "ds-nonfinite", "ds-gs-alias")
                                                             else (0, 0, 0, 0)), "original storage")
    require(result["after_sha256"] == storage_hash((0, 0, 0, 0) if mode == "ds-upstream-alias" else
                                                       (3, 0, 0, 0) if gs else (42, 43, 52, 53)),
            "independent draw storage")
    require(result["counter_before"] == 0 and result["counter_after"] == (0 if gs or mode == "ds-upstream-alias" else 4), "counter values")
    full = result["full"]
    if mode == "ds-gs-alias":
        require(12 <= full["vertices"] <= 36 and full["vertices"] == full["primitives"] * 3, "DS-fed GS bounds")
    elif rejected and gs:
        require(9 <= full["vertices"] <= 27 and full["vertices"] == full["primitives"] * 3, "aliased geometry bounds")
    else:
        require(full["vertices"] == (9 if unbound or mode == "gs-empty" else 18 if gs else 12) and
                full["primitives"] == (3 if unbound or mode == "gs-empty" else 6 if gs else 4), "full geometry")
    if terminal_domain:
        words = result["full_words"]
        require(len(words) == full["vertices"] * 4 and all(type(w) is int and 0 <= w <= 0xffffffff for w in words),
                "raw full geometry size")
        raw = struct.pack("<" + "I" * len(words), *words)
        require(hashlib.sha256(raw).hexdigest() == full["sha256"], "raw full geometry digest")
        if mode == "ds-nonfinite":
            require(all(w == 0x7f800000 for w in words[::4]), "SO captures pre-clipping infinity")
        if mode == "ds-atomic":
            positions = list(struct.iter_unpack("<4f", raw))
            for instance in range(2):
                expected_positions = [(42.0 + i, float(instance), (i % 3) * .25 + i // 3, 1.0) for i in range(6)]
                require(sorted(positions[instance * 6:(instance + 1) * 6]) == expected_positions,
                        "live atomic result and original vertex inputs")
        identity = result["pixel_geometry"]["geometry_instances"]
        require(identity["status"] == ("unavailable" if rejected else "resolved") and
                identity["geometry_sha256"] == full["sha256"] and
                identity["original_fragment_eligibility"] is None and identity["original_gpu_schedule"] is False,
                "captured geometry partition scope")
        rejections = result["selection_rejections"]
        require(len(rejections) == ((3 if gs else 2) if rejected else 0), "rejection evidence")
        reason = "IA/CB input aliases a written UAV" if mode == "ia-alias" else "Read resource aliases a written UAV"
        require(all(r["instance"] == i and reason in r["reason"] for i, r in enumerate(rejections)), "rejection reason")
    selections = result["selections"]
    if rejected:
        require(not selections, "unproved selection must be rejected")
        return
    require(len(selections) == (3 if gs else 2), "all instances")
    for i, selection in enumerate(selections):
        first, count = ((i * (i - 1) // 2 * 3, i * 3) if mode == "gs-empty" else
                        (i * 3, 3) if unbound else (i * (i + 1) // 2 * 3, (i + 1) * 3) if gs else (i * 6, 6))
        require(selection["instance"] == i and selection["instance_count"] == len(selections), "instance")
        require(selection["first_vertex"] == first and selection["vertex_count"] == count, "slice bounds")
        if terminal_domain:
            require(selection["selected_sha256"] == hashlib.sha256(raw[first * 16:(first + count) * 16]).hexdigest(),
                    "original full capture slice bytes")
        require(selection["full_capture"] == {k: full[k] for k in ("vertices", "primitives", "sha256")},
                "full capture identity")
        require(selection["strategy"] == "native_prefix_statistics" and
                selection["system_ids_rewritten"] is False and selection["input_offsets_rewritten"] is False,
                "unmodified draw parameters")
        queries = selection["prefix_queries"]
        expected = [n for n in (i, i + 1) if 0 < n < len(selections)]
        require([q["instance_count"] for q in queries] == expected, "prefix sequence")
        for q in queries:
            n = q["instance_count"]
            count = n * (n - 1) // 2 if mode == "gs-empty" else n if unbound else n * (n + 1) // 2 if gs else n * 2
            require(q["primitives"] == count and q["attempts"][-1]["primitives_storage_needed"] == count,
                    "prefix query count")
        proof = selection["uav_independence"]
        if terminal_domain:
            require(proof == result["pixel_geometry"]["geometry_instances"]["uav_independence"], "shared partition proof")
        require(proof["input_aliases_excluded"] is True, "actual input ownership")
        require(set(proof["stages"]) == ({"vs", "gs"} if gs else {"vs", "hs", "ds"}), "executed stages")
        reads = mode in ("gs-read-uav", "gs-read-srv", "gs-unbound")
        expected_reads = []
        if reads:
            kind, slot = ("srv_slots", 0) if mode == "gs-read-srv" else ("uav_slots", 2)
            expected_reads = [{"stage": "gs", "kind": kind, "slot": slot, "view": 0 if unbound else 9002,
                               "aliases_write": False}]
        elif mode == "read":
            expected_reads = [{"stage": "ds", "kind": "uav_slots", "slot": 1, "view": 12, "aliases_write": True}]
        if terminal_domain:
            for read in expected_reads:
                read["count_dependency"] = read["stage"] != "ds"
        if reads or terminal_domain:
            require(proof["write_slots"] == [1] and proof["resource_reads"] == expected_reads, "runtime read ownership")
        for stage, item in proof["stages"].items():
            terminal = terminal_domain and stage == "ds"
            require(item.get("terminal_domain_counts", False) is terminal, "terminal stage classification")
            scope = ("terminal_domain_counts" if terminal else "immutable_resource_inputs" if reads and stage == "gs"
                     else "no_uav_results_or_srv_reads")
            require(item["proved"] is True and item["scope"] == scope, "stage proof")
            dependencies = {"srv_slots": [], "uav_slots": []}
            if reads and stage == "gs":
                dependencies[kind] = [slot]
            elif mode == "read" and stage == "ds":
                dependencies["uav_slots"] = [1]
            if terminal_domain or (reads and stage == "gs"):
                require(item["read_dependencies"] == dependencies, "static read dependencies")
            require(re.fullmatch("[0-9a-f]{64}", item["shader_sha256"]) and
                    0 <= item["liveness_instructions_checked"] <= 1048576, "bounded shader proof")
            writes = item["write_instructions"]
            expected = [173] if stage == "gs" else [178, 168] if stage == "hs" else [182] if stage == "ds" and mode in ("ds-atomic", "ds-nonfinite") else []
            require([w["opcode"] for w in writes] == expected, "fixture operations")
            require(all(w["slot"] == 1 and w["result"] == ("terminal_domain_value" if w["opcode"] == 182 else
                                                           "proved_dead" if w["opcode"] == 178 else "none")
                        for w in writes), "write results")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--resource-reads", action="store_true", help="Require the expanded immutable-read matrix")
    parser.add_argument("--empty-instance", action="store_true", help="Also require variable GS output with an empty first instance")
    parser.add_argument("--terminal-domain", action="store_true", help="Require terminal DS counts and upstream/GS rejection controls")
    parser.add_argument("--fixed-geometry", action="store_true", help="Require the suite including fixed GS count tests")
    args = parser.parse_args()
    if args.terminal_domain:
        args.resource_reads = args.empty_instance = True
    paths = {p.stem: p for p in args.directory.glob("*.json")}
    expected = {f"{v}-{mode}" for v in (0, 4318, 32902) for mode in ("independent", "gs", "read", "ia-alias")}
    if args.resource_reads:
        expected |= {f"{v}-{mode}" for v in (0, 4318, 32902)
                     for mode in ("gs-read-uav", "gs-read-srv", "gs-unbound", "gs-alias-uav", "gs-alias-srv")}
    if args.empty_instance:
        expected |= {f"{v}-gs-empty" for v in (0, 4318, 32902)}
    if args.terminal_domain:
        expected |= {f"{v}-{mode}" for v in (0, 4318, 32902) for mode in
                     ("ds-atomic", "ds-nonfinite", "ds-upstream-alias", "ds-gs-alias")}
    require(paths.keys() == expected, "complete evidence matrix")
    passed = 53 if args.resource_reads else 31
    if args.empty_instance:
        passed += 3
    if args.terminal_domain:
        passed += 13
    if args.fixed_geometry:
        passed += 16
    require(re.search(rf"Totals: {passed} passed, 0 failed, 0 skipped, 0 blacklisted,",
                      args.log.read_text(encoding="utf-8")), "complete Qt suite")
    controls = 0
    for name, path in sorted(paths.items()):
        result = json.loads(path.read_text(encoding="utf-8"))
        validate(name, result, args.terminal_domain)
        mutations = [lambda r: r.update(counter_after=99), lambda r: r.update(storage_restored=False),
                     lambda r: r.update(after_sha256="0" * 64)]
        if result["selections"]:
            mutations += [lambda r: r["selections"][0].update(first_vertex=999),
                          lambda r: r["selections"][0]["uav_independence"].update(input_aliases_excluded=False)]
        if args.terminal_domain:
            mutations += [lambda r: r["full_words"].__setitem__(0, r["full_words"][0] ^ 1)]
            if result["selections"]:
                mutations += [lambda r: r["selections"][0].update(selected_sha256="0" * 64),
                              lambda r: r["selections"][0]["uav_independence"]["stages"]["vs"].update(terminal_domain_counts=True)]
            else:
                mutations += [lambda r: r["selection_rejections"][0].update(reason="Unsupported")]
        for mutate in mutations:
            changed = copy.deepcopy(result)
            mutate(changed)
            try:
                validate(name, changed, args.terminal_domain)
            except ValueError:
                controls += 1
            else:
                raise ValueError("Accepted corruption in " + name)
    summary = {"schema": 1, "status": "ok", "cases": len(paths), "corruption_controls": controls,
               "full_uav_instance_support": False, "full_renderdoc_removal": False}
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(summary, output, indent=2)
        output.write("\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
