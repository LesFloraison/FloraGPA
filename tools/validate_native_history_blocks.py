"""Verify retained native BC history probes against their captured source bytes.

The probe corpus covers all BC1-BC7 format variants on three explicit adapters.
This checks raw storage and event provenance, not fragment or floating-point parity.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import struct


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(folder, report=None, capture=None):
    report = report if report is not None else json.loads((folder / "observations.json").read_text("utf-8"))
    capture = capture if capture is not None else (folder / "source.gpa_frame").read_bytes()
    vendor, fmt, cube = map(int, folder.name.split("-"))
    require((report["vendor"], report["format"], report["cube"]) == (vendor, fmt, bool(cube)), "case identity")
    require(struct.unpack_from("<3I", capture) == (0x41504749, 0x128, 3), "capture header")
    count = struct.unpack_from("<I", capture, 12)[0]
    directory = struct.unpack_from("<Q", capture, 0xf4)[0]
    require(count == 10 and directory + count * 24 == len(capture), "capture directory")
    entries = {}
    for i in range(count):
        ident, offset, size, flags, category, kind = struct.unpack_from("<QQIBBH", capture, directory + i * 24)
        require(not flags and 0x128 <= offset <= offset + size <= directory and ident not in entries,
                "capture entry bounds")
        entries[ident] = (category, kind, capture[offset:offset + size])
    require(set(entries) == {1, 10, 11, 12, 30, 31, 32, 90, 100, 110}, "capture identities")
    desc = (12, 12 if cube else 20, 4 if cube else 5, 6 if cube else 2, fmt, 1, 0, 0, 8, 0, 4 if cube else 0)
    for ident in (10, 11, 12):
        category, kind, payload = entries[ident]
        require((category, kind, len(payload)) == (5, 0x85, 68), "texture descriptor")
        require(struct.unpack_from("<11I", payload, 16) == desc and
                struct.unpack_from("<Q", payload, 60)[0] == ident + 20, "texture layout")
    for event, dst, src in ((90, 12, 10), (100, 10, 11), (110, 10, 11)):
        category, kind, payload = entries[event]
        require((category, kind) == (7, 0x3e) and struct.unpack("<4Q", payload) == (0, 1, dst, src),
                "copy command")
    block = 8 if fmt in (*range(70, 73), *range(79, 82)) else 16
    offsets, offset = {}, 0
    for layer in range(desc[3]):
        for mip in range(desc[2]):
            width, height = max(1, desc[0] >> mip), max(1, desc[1] >> mip)
            row_pitch = ((width + 3) // 4) * block
            for x, y in {(0, 0), (width // 2, height // 2), (width - 1, height - 1)}:
                offsets[(layer, mip, x, y)] = offset + (y // 4) * row_pitch + (x // 4) * block
            offset += row_pitch * ((height + 3) // 4)
    assets = {}
    for ident in (30, 31, 32):
        category, kind, payload = entries[ident]
        require((category, kind, len(payload)) == (9, 1, offset + 4) and
                struct.unpack_from("<I", payload)[0] == offset, "source asset length")
        assets[ident] = payload[4:]
    require(assets[30] == assets[32] == bytes(offset), "initial zero storage")
    require(assets[31] == bytes((i * 53 + (i // 256) * 31 + 19) % 256 for i in range(offset)),
            "independent source pattern")
    seen = set()
    for history in report["observations"]:
        s = history["selection"]
        key = s["layer"], s["mip"], s["x"], s["y"]
        require(key in offsets and key not in seen, "mip/layer/edge coverage")
        seen.add(key)
        require(s["resource"] == 10 and s["sample"] == s["slice"] == 0 and s["typed_format"] is None,
                "pixel selection")
        require(history["schema"] == 2 and history["kind"] == "pixel_event_history" and
                history["source"] == "native_dx11_replay" and history["replay_count"] == 1 and
                history["fragment_history_available"] is False, "history provenance")
        require((history["readback_count"], history["point_readback_count"], history["block_readback_count"],
                 history["readback_bytes"]) == (4, 0, 4, 4 * block), "bounded block readback")
        require([r["event"] for r in history["records"]] == [100, 110], "target write attribution")
        start = offsets[key]
        wanted = assets[31][start:start + block].hex()
        for record in history["records"]:
            require(record["record_kind"] == "event_snapshot" and record["enabled"] is True, "record kind")
            require(all(record[k] is None for k in ("fragment", "shader_output", "failure_reasons",
                                                    "write_executed", "pixel_storage_changed")),
                    "unknown fragment and execution evidence")
            require(record["storage_changed"] is (record["event"] == 100), "changed/unchanged storage")
            for boundary in ("before", "after"):
                snap = record[boundary]
                expected = "00" * block if record["event"] == 100 and boundary == "before" else wanted
                require(snap["available"] is True and snap["defined"] is None and snap["bytes"] == expected,
                        "captured boundary bytes")
                require(snap["format"] == fmt and snap["encoding"] == "compressed_4x4_block" and
                        snap["readback_scope"] == "single_compressed_block" and
                        snap["exact_pixel_storage"] is False, "block scope")
    require(seen == set(offsets), "complete selection coverage")
    return {"case": folder.name, "histories": len(seen), "snapshots": len(seen) * 4,
            "readback_bytes": len(seen) * 4 * block,
            "equivalent_complete_readback_bytes": len(seen) * 4 * offset,
            "capture_sha256": digest(folder / "source.gpa_frame"),
            "observations_sha256": digest(folder / "observations.json")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    require(not args.out.exists(), "use a new output report")
    expected = {f"{vendor}-{fmt}-{cube}" for vendor in (0, 4318, 32902)
                for fmt in (*range(70, 85), *range(94, 100))
                for cube in ((0, 1) if fmt == 71 else (0,))}
    require({p.name for p in args.evidence.iterdir()} == expected, "66 explicit adapter/format cases")
    cases = [validate(args.evidence / name) for name in sorted(expected)]
    folder = args.evidence / "0-71-0"
    original = json.loads((folder / "observations.json").read_text("utf-8"))
    controls = []
    for name in ("changed_bytes", "claimed_texel", "missing_block_count", "duplicate_selection", "defined_value"):
        mutated = copy.deepcopy(original)
        history = mutated["observations"][0]
        snap = history["records"][0]["after"]
        if name == "changed_bytes":
            snap["bytes"] = "00" + snap["bytes"][2:]
        elif name == "claimed_texel":
            snap["exact_pixel_storage"] = True
        elif name == "missing_block_count":
            history["block_readback_count"] = 0
        elif name == "duplicate_selection":
            mutated["observations"].append(copy.deepcopy(history))
        else:
            snap["defined"] = True
        try:
            validate(folder, mutated)
        except AssertionError:
            controls.append(name)
        else:
            raise AssertionError(f"corruption accepted: {name}")
    log = args.log.read_text("utf-8")
    require(not re.search(r"FAIL!|SKIP|QFATAL", log), "regression log failures")
    totals = re.search(r"Totals: (\d+) passed, 0 failed, 0 skipped", log)
    require(totals is not None, "completed regression suite")
    result = {"schema": 1, "ok": True, "complete_release_acceptance": False, "cases": cases,
              "rejected_controls": controls, "qt_passes": int(totals[1]), "log_sha256": digest(args.log)}
    for key in ("histories", "snapshots", "readback_bytes", "equivalent_complete_readback_bytes"):
        result[key] = sum(case[key] for case in cases)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k != "cases"}, indent=2))


if __name__ == "__main__":
    main()
