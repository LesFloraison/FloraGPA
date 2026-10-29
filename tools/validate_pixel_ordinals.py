"""Validate owned stencil-ordinal observations and pinned GPA counter evidence."""
import argparse
import copy
import hashlib
import itertools
import json
import re
import struct
import sys
from pathlib import Path

PLAYER = "39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b"
RANKS = [-1, 0, 1, 2, 6, 7, 8, 127, 253, 254, 255]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expected_fragment(mode, sample, count, rank):
    # Closed-form selection, independent of the probe's incremental stencil model.
    if mode in (1, 3):
        parity = sample % 2 if mode == 3 else 0
        survivors = (count + 1 - parity) // 2
        selected = survivors - 1 if rank == -1 or (rank == 255 and survivors > 255) else rank
        return parity + 2 * selected if 0 <= selected < survivors else None
    selected = count - 1 if rank == -1 or (rank == 255 and count > 255) else rank
    if not 0 <= selected < count:
        return None
    if mode == 2 and selected % 2:
        if rank == -1 or (rank == 255 and count > 255):
            selected -= 1
        else:
            return None
    return selected


def validate(report, key):
    vendor, layout, samples, mode, targets = key
    require(report["schema"] == 1 and report["scope"] == "owned_draw_stencil_ordinal_probe", "Wrong scope")
    require(report["original_fragment_eligibility"] is None, "Unproven fragment qualification")
    require([report[k] for k in ("requested_vendor", "layout", "samples", "mode", "targets")] == list(key),
            "Selection identity differs")
    require(report["actual_vendor"] == (vendor or 5140), "Wrong actual adapter")
    require(report["actual_device"] > 0 and len(report["adapter_luid"]) == 2, "Missing device identity")
    require(not report["diagnostics"], "GPU validation diagnostics")
    require([(r["count"], r["rank"]) for r in report["rows"]] == list(itertools.product((8, 260), RANKS)),
            "Missing, duplicated or reordered observations")
    vectors = 0
    for row in report["rows"]:
        count, rank = row["count"], row["rank"]
        expected = []
        for sample in range(samples):
            fragment = expected_fragment(mode, sample, count, rank)
            expected.append([0, 0, 0, 0] if fragment is None else
                            [fragment + 1, 7 if layout == 2 else 0 if layout == 1 else fragment,
                             fragment if layout == 1 else 0, sample])
        require(row["outputs"] == expected == row["expected"], "Fragment selection differs")
        require(type(row["occlusion"]) is int and row["occlusion"] >= 0, "Invalid occlusion value")
        vectors += samples
    return vectors


def query_failures(report):
    failures = []
    if report["mode"] == 0:
        for row in report["rows"]:
            count, rank = row["count"], row["rank"]
            passing = count if rank < 0 else max(count - 255, 0) if rank == 255 else int(rank < count)
            expected = 16 * report["samples"] * passing
            if row["occlusion"] != expected:
                failures.append({"count": count, "rank": rank, "actual": row["occlusion"], "expected": expected})
    return failures


def signatures(path):
    data = path.read_bytes()
    require(data[:4] == b"DXBC" and struct.unpack_from("<I", data, 24)[0] == len(data), "Invalid shader size")
    result = {}
    for i in range(struct.unpack_from("<I", data, 28)[0]):
        offset = struct.unpack_from("<I", data, 32 + 4 * i)[0]
        tag, size = struct.unpack_from("<4sI", data, offset)
        chunk = data[offset + 8:offset + 8 + size]
        require(len(chunk) == size, "Truncated shader chunk")
        if tag not in (b"ISGN", b"OSGN", b"OSG5"):
            continue
        entries = {}
        for index in range(struct.unpack_from("<I", chunk)[0]):
            stride = 28 if tag == b"OSG5" else 24
            fields = struct.unpack_from("<" + "I" * (stride // 4), chunk, 8 + stride * index)
            if stride == 28:
                require(fields[0] == 0, "Unexpected GS stream")
                fields = fields[1:]
            name, semantic, system, component, register, masks = fields
            key = (chunk[name:chunk.index(b"\0", name)].decode("ascii"), semantic)
            require(key not in entries, "Duplicate shader semantic")
            entries[key] = (system, component, register, masks & 255)
        result[tag] = entries
    return result


def static_evidence(root, sdk):
    constants = (root / "constants.txt").read_text(encoding="utf-8")
    calls = (root / "counter.txt").read_text(encoding="utf-8")
    binary = json.loads((root / "binary.json").read_text(encoding="utf-8-sig"))
    require(binary["sha256"] == PLAYER and all(PLAYER in text.splitlines()[0] for text in (constants, calls)),
            "Player identity differs")
    require(constants.rstrip().endswith("COMPLETE 2") and calls.rstrip().endswith("COMPLETE 4"),
            "Incomplete Ghidra export")
    words = {}
    for rva in ("17d220", "17d230"):
        block = constants.split("ADDRESS RVA 0x" + rva + "\n", 1)[1].split("ADDRESS RVA ", 1)[0]
        raw = b"".join(struct.pack("<Q", int(re.search(r"^QWORD \+0x" + f"{offset:x}" + r" ([0-9a-f]+)$",
                                                       block, re.M)[1], 16)) for offset in (0, 8))
        words[rva] = list(struct.unpack("<4I", raw))
    require(words == {"17d220": [2, 1, 1, 1], "17d230": [0, 0, 1, 2]}, "Counter constants differ")
    instructions = [
        "0x4dec4 XOR R12D,R12D",
        "0x4e0a2 MOVDQA XMM0,xmmword ptr [0x18017d230]",
        "0x4e0aa MOVDQA xmmword ptr [RBP + 0x100],XMM0",
        "0x4e0b2 MOVDQA XMM1,xmmword ptr [0x18017d220]",
        "0x4e0ba MOVDQA xmmword ptr [RBP + 0x110],XMM1",
        "0x4e0c2 MOV dword ptr [RBP + 0x120],0x1",
        "0x4e0cc MOV byte ptr [RBP + 0x124],0xf",
        "0x4e21c LEA R8,[RBX + 0xf8]",
        "0x4e223 LEA RDX,[RBP + 0x100]",
        "0x4e22a CALL qword ptr [RAX + 0xa0]",
        "0x4e250 MOV dword ptr [RBP + 0x108],R12D",
        "0x4e257 MOV byte ptr [RBP + 0x124],0x0",
        "0x4e2ca LEA R8,[RBX + 0x100]",
        "0x4e2d8 CALL qword ptr [RAX + 0xa0]",
        "0x48851 MOV RDX,qword ptr [RBX + 0xf8]",
        "0x4885b CALL 0x18006b920",
        "0x4887b CALL qword ptr [RAX + 0x70]",
        "0x48884 CALL 0x180043b70",
        "0x48889 MOV dword ptr [RBX + 0x174],EAX",
        "0x43ca8 CVTTSS2SI RSI,dword ptr [RAX]",
        "0x6b959 JMP qword ptr [RAX + 0x118]",
        "0x6b983 JMP qword ptr [RAX + 0x118]",
    ]
    require(all("ASM RVA " + line in calls for line in instructions), "Counter instruction evidence differs")
    header = sdk.read_text(encoding="utf-8-sig")
    slots = {}
    for interface, method, expected in (("ID3D11Device", "CreateBlendState", 0xa0),
                                         ("ID3D11DeviceContext", "OMSetBlendState", 0x118)):
        block = header.split("typedef struct " + interface + "Vtbl", 1)[1].split("END_INTERFACE", 1)[0]
        names = re.findall(r"\(\s*STDMETHODCALLTYPE\s*\*(\w+)\s*\)", block)
        slots[interface + "::" + method] = names.index(method) * 8
        require(names.index(method) * 8 == expected, "SDK slot differs")
    for name, value in (("D3D11_BLEND_ONE", 2), ("D3D11_BLEND_ZERO", 1), ("D3D11_BLEND_OP_ADD", 1)):
        require(re.search(r"\b" + name + r"\s*=\s*" + str(value) + r"\b", header), "SDK blend enum differs")
    return {"binary": binary, "constants_u32": words, "sdk_sha256": digest(sdk), "slots": slots,
            "rt0": {"blend_enabled": True, "source_rgb": "one", "destination_rgb": "one",
                    "rgb_operation": "add", "source_alpha": "zero", "destination_alpha": "zero",
                    "alpha_operation": "add", "write_mask": 15},
            "complete_fragment_history": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--ghidra", required=True, type=Path)
    parser.add_argument("--sdk-header", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    keys = list(itertools.product((0, 4318, 32902), range(3), (1, 4), range(4), (1, 8)))
    require({p.name for p in args.root.iterdir() if p.is_dir()} == {"-".join(map(str, k)) for k in keys},
            "Unexpected matrix directories")
    vectors = controls = 0
    query_errors = []
    shaders = {}
    for key in keys:
        folder = args.root / "-".join(map(str, key))
        report = json.loads((folder / "results.json").read_text(encoding="utf-8"))
        vectors += validate(report, key)
        failures = query_failures(report)
        if failures:
            query_errors.append({"key": key, "observations": failures})
        if report["mode"] == 0:
            changed = copy.deepcopy(report)
            next(r for r in changed["rows"] if r["count"] == 8 and r["rank"] == 8)["occlusion"] = 1
            require(len(query_failures(changed)) == len(failures) + 1, "Altered empty query was accepted")
            controls += 1
        for filename in ("vs.dxbc", "gs.dxbc", "ps.dxbc", "readback.dxbc"):
            shader_key = (key[1:], filename)
            digest_now = digest(folder / filename)
            require(shaders.setdefault(shader_key, digest_now) == digest_now, "Cross-adapter shader differs")
        gs = signatures(folder / "gs.dxbc")[b"OSG5"]
        ps = signatures(folder / "ps.dxbc")[b"ISGN"]
        for semantic in (("SV_Position", 0), ("TEXCOORD", 0), ("SV_PrimitiveID", 0)):
            require(gs[semantic] == ps[semantic], "GS/PS register linkage differs")
        for change in range(6):
            bad = copy.deepcopy(report)
            if change == 0:
                bad["original_fragment_eligibility"] = True
            elif change == 1:
                bad["rows"][1]["outputs"][0][0] += 1
            elif change == 2:
                bad["rows"].pop()
            elif change == 3:
                bad["diagnostics"].append({"text": "linkage error"})
            elif change == 4:
                bad["actual_vendor"] = 12345
            else:
                bad["rows"][1]["outputs"][0][3] += 1
            try:
                validate(bad, key)
            except ValueError:
                controls += 1
            else:
                raise ValueError("Corrupt ordinal evidence was accepted")
    result = {"schema": 1, "scope": "owned_stencil_ordinal_and_gpa_counter_research",
              "complete_release_acceptance": False, "groups": len(keys), "draws": len(keys) * 22,
              "sample_vectors": vectors, "corruption_controls": controls,
              "strict_contract_passed": not query_errors, "query_contract_failures": query_errors,
              "gpa_counter": static_evidence(args.ghidra, args.sdk_header),
              "artifacts": [{"path": str(p), "bytes": p.stat().st_size, "sha256": digest(p)}
                            for root in (args.root, args.ghidra) for p in sorted(root.rglob("*")) if p.is_file()]}
    with args.out.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps({k: v for k, v in result.items()
                      if k not in ("artifacts", "gpa_counter", "query_contract_failures")}))
    return int(bool(query_errors))


if __name__ == "__main__":
    sys.exit(main())
