"""Validate active PS typed UAV operations against native PS and reversible journals."""
import argparse
import copy
import json
import re
import struct
from pathlib import Path


def require(value, message):
    if not value:
        raise ValueError(message)


def validate(name, r):
    _, vendor, dimension, fmt, mode = name.split("-")
    vendor, dimension, fmt, mode = map(int, (vendor, dimension, fmt, mode))
    require(r["schema"] == 1 and r["status"] == "ok" and
            (r["vendor"], r["dimension"], r["format"], r["mode"]) == (vendor, dimension, fmt, mode), "case identity")
    for key in ("helper_accesses_suppressed", "journal_roundtrip", "original_storage_restored"):
        require(r[key] is True, key)
    require(r["valid_mask"] == 15 and len(r["gpu_color"]) == 16 and
            r["raw_bits"] == list(struct.unpack("<4I", bytes(r["gpu_color"]))), "native PS output")
    storage = bytearray([0x11]*len(r["gpu_storage"]))
    transactions = r["transactions"]
    require(len(transactions) == 8 and [t["lane"] for t in transactions] == [0,1,2,3]*2, "one store/load quad")
    changed = []
    for t in transactions:
        require(t["memory_gpu_assisted"] is (t["lane"] == 0), "PS assistance and helper suppression")
        require(not t["counter_changes"] and not t["shared_changes"] and t["atomic"] is False, "typed access class")
        changes = t["buffer_changes"] + t["texture_changes"]
        if t["lane"]:
            require(not changes, "helper writes")
            for reg in t["register_changes"]:
                require(reg["after"]["valid_components"] == 0, "helper load validity")
        require(not (t["texture_changes"] if dimension == 1 else t["buffer_changes"]), "resource change kind")
        for c in changes:
            offset = c["offset"]
            before, after = bytes.fromhex(c["before_hex"]), bytes.fromhex(c["after_hex"])
            require(c["resource"] == 10 and len(before) == len(after) > 0 and
                    0 <= offset <= len(storage)-len(before), "store bounds")
            require(bytes.fromhex(c["before_valid_hex"]) == bytes([1])*len(before) ==
                    bytes.fromhex(c["after_valid_hex"]), "store validity")
            require(storage[offset:offset+len(before)] == before, "store preimage")
            storage[offset:offset+len(after)] = after
            changed.append(c)
    require(list(storage) == r["gpu_storage"], "native PS storage comparison")
    require(len(changed) <= 1 and (mode != 0 or len(changed) == 1), "one selected element")
    for c in reversed(changed):
        offset=c["offset"];before=bytes.fromhex(c["before_hex"])
        storage[offset:offset+len(before)] = before
    require(storage == bytearray([0x11]*len(storage)), "reverse storage journal")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory",type=Path)
    parser.add_argument("--log",type=Path,required=True)
    parser.add_argument("--out",type=Path,required=True)
    args=parser.parse_args()
    expected={f"typedPixelReadWrite-{v}-{d}-{f}-{m}" for v in (0,4318,32902)
              for d in (1,2,3,5,7,8) for f in (42,43,41,28,31,10) for m in (0,1,2)}
    paths={p.stem:p for p in args.directory.glob("typedPixelReadWrite-*.json")}
    require(paths.keys()==expected,"complete 324-case matrix")
    require(re.search(r"Totals: 635 passed, 0 failed, 0 skipped, 0 blacklisted,",args.log.read_text(encoding="utf-8")),
            "full checkpoint Qt suite")
    controls=0
    for name in sorted(expected):
        r=json.loads(paths[name].read_text(encoding="utf-8"));validate(name,r)
        mutations=[lambda b:b["raw_bits"].__setitem__(0,b["raw_bits"][0]^1),
                   lambda b:b["transactions"][1].update(memory_gpu_assisted=True),
                   lambda b:b["gpu_storage"].__setitem__(-1,b["gpu_storage"][-1]^1),
                   lambda b:b.update(journal_roundtrip=False)]
        for mutate in mutations:
            b=copy.deepcopy(r);mutate(b)
            try:validate(name,b)
            except ValueError:controls+=1
            else:raise ValueError("Accepted corruption: "+name)
    result=dict(schema=1,status="ok",data_cases=len(expected),corruption_controls=controls,
                scope="active_pixel_typed_uav_load_store",public_pixel_debugging=False,full_renderdoc_removal=False)
    with args.out.open("x",encoding="utf-8") as file:json.dump(result,file,indent=2);file.write("\n")
    print(json.dumps(result))


if __name__=="__main__":main()
