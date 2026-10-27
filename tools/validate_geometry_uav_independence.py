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


def validate(name, result):
    vendor, mode = name.split("-", 1)
    require(result["schema"] == 1 and result["vendor"] == int(vendor) and result["mode"] == mode, "identity")
    for key in ("storage_restored", "counter_restored", "clean_draw_equal"):
        require(result[key] is True, key)
    gs = mode.startswith("gs")
    unbound = mode == "gs-unbound"
    rejected = mode == "read" or "alias" in mode
    require(result["before_sha256"] == storage_hash((0, 0, 0, 0)), "original storage")
    require(result["after_sha256"] == storage_hash((3, 0, 0, 0) if gs else (42, 43, 52, 53)),
            "independent draw storage")
    require(result["counter_before"] == 0 and result["counter_after"] == (0 if gs else 4), "counter values")
    full = result["full"]
    if rejected and gs:
        require(9 <= full["vertices"] <= 27 and full["vertices"] == full["primitives"] * 3, "aliased geometry bounds")
    else:
        require(full["vertices"] == (9 if unbound else 18 if gs else 12) and
                full["primitives"] == (3 if unbound else 6 if gs else 4), "full geometry")
    selections = result["selections"]
    if rejected:
        require(not selections, "unproved selection must be rejected")
        return
    require(len(selections) == (3 if gs else 2), "all instances")
    for i, selection in enumerate(selections):
        first, count = (i * 3, 3) if unbound else (i * (i + 1) // 2 * 3, (i + 1) * 3) if gs else (i * 6, 6)
        require(selection["instance"] == i and selection["instance_count"] == len(selections), "instance")
        require(selection["first_vertex"] == first and selection["vertex_count"] == count, "slice bounds")
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
            count = n if unbound else n * (n + 1) // 2 if gs else n * 2
            require(q["primitives"] == count and q["attempts"][-1]["primitives_storage_needed"] == count,
                    "prefix query count")
        proof = selection["uav_independence"]
        require(proof["input_aliases_excluded"] is True, "actual input ownership")
        require(set(proof["stages"]) == ({"vs", "gs"} if gs else {"vs", "hs", "ds"}), "executed stages")
        reads = mode in ("gs-read-uav", "gs-read-srv", "gs-unbound")
        if reads:
            kind, slot = ("srv_slots", 0) if mode == "gs-read-srv" else ("uav_slots", 2)
            require(proof["write_slots"] == [1] and proof["resource_reads"] ==
                    [{"stage": "gs", "kind": kind, "slot": slot, "view": 0 if unbound else 9002,
                      "aliases_write": False}], "runtime read ownership")
        for stage, item in proof["stages"].items():
            require(item["proved"] is True and item["scope"] ==
                    ("immutable_resource_inputs" if reads and stage == "gs" else "no_uav_results_or_srv_reads"), "stage proof")
            if reads and stage == "gs":
                require(item["read_dependencies"] == {"srv_slots": [0] if kind == "srv_slots" else [],
                                                       "uav_slots": [2] if kind == "uav_slots" else []},
                        "static read dependencies")
            require(re.fullmatch("[0-9a-f]{64}", item["shader_sha256"]) and
                    0 <= item["liveness_instructions_checked"] <= 1048576, "bounded shader proof")
            writes = item["write_instructions"]
            expected = [173] if stage == "gs" else [178, 168] if stage == "hs" else []
            require([w["opcode"] for w in writes] == expected, "fixture operations")
            require(all(w["slot"] == 1 and w["result"] == ("proved_dead" if w["opcode"] == 178 else "none")
                        for w in writes), "write results")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--resource-reads", action="store_true", help="Require the expanded immutable-read matrix")
    args = parser.parse_args()
    paths = {p.stem: p for p in args.directory.glob("*.json")}
    expected = {f"{v}-{mode}" for v in (0, 4318, 32902) for mode in ("independent", "gs", "read", "ia-alias")}
    if args.resource_reads:
        expected |= {f"{v}-{mode}" for v in (0, 4318, 32902)
                     for mode in ("gs-read-uav", "gs-read-srv", "gs-unbound", "gs-alias-uav", "gs-alias-srv")}
    require(paths.keys() == expected, "complete evidence matrix")
    passed = 53 if args.resource_reads else 31
    require(re.search(rf"Totals: {passed} passed, 0 failed, 0 skipped, 0 blacklisted,",
                      args.log.read_text(encoding="utf-8")), "complete Qt suite")
    controls = 0
    for name, path in sorted(paths.items()):
        result = json.loads(path.read_text(encoding="utf-8"))
        validate(name, result)
        mutations = [lambda r: r.update(counter_after=99), lambda r: r.update(storage_restored=False),
                     lambda r: r.update(after_sha256="0" * 64)]
        if result["selections"]:
            mutations += [lambda r: r["selections"][0].update(first_vertex=999),
                          lambda r: r["selections"][0]["uav_independence"].update(input_aliases_excluded=False)]
        for mutate in mutations:
            changed = copy.deepcopy(result)
            mutate(changed)
            try:
                validate(name, changed)
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
