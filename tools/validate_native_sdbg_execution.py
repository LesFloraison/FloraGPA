"""Check original SDBG input records and saved native source-state observations."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct


def require(condition, message):
    if not condition:
        raise ValueError(message)


def metadata(raw):
    require(raw[:4] == b"DXBC", "Shader container")
    count = struct.unpack_from("<I", raw, 28)[0]
    found = []
    for offset in struct.unpack_from("<" + "I" * count, raw, 32):
        if raw[offset:offset + 4] == b"SDBG":
            size = struct.unpack_from("<I", raw, offset + 4)[0]
            found.append(raw[offset + 8:offset + 8 + size])
    require(len(found) == 1, "One original SDBG chunk")
    header = struct.unpack_from("<21I", found[0])
    base = found[0][84:]

    def table(index, width):
        offset, count = header[6 + index * 2], header[5 + index * 2]
        require(offset <= len(base) and count <= (len(base) - offset) // (width * 4), "SDBG table extent")
        return [struct.unpack_from("<" + "I" * width, base, offset + i * width * 4) for i in range(count)]

    scalars, tokens = table(2, 6), table(4, 5)
    strings = base[header[20]:]
    inputs = []
    for record in table(3, 6):
        scalar = scalars[record[0]]
        token = tokens[scalar[0]]
        name = strings[token[4]:token[4] + token[3]].decode("utf-8")
        inputs.append({"name": name, "record": record, "type": scalar[1]})
    return inputs, table(1, 99), scalars


def check(stage, shader, model, observations, trace):
    require(model["shader_sha256"] == hashlib.sha256(shader).hexdigest(), "Shader identity")
    symbols = model["source_variables"]
    require(symbols["format"] == "SDBG assignments" and symbols["status"] == "available", "Available SDBG")
    inputs, instructions, scalars = metadata(shader)
    observed = {v["name"]: v for v in observations[0]["values"]}
    expected = ({"global.x": 5, "group.y": 3, "local.y": 1, "flat": 3} if stage == "cs" else
                {"position.x": 0x40200000, "vertex": 7, "instance": 3} if stage == "vs" else
                {"position.x": 0x40200000, "primitive": 7, "front": 0xffffffff, "sample": 2})
    for name, bits in expected.items():
        require(observed[name]["status"] == "available" and observed[name]["bits"] == bits,
                "Original input value " + name)
    if stage == "cs":
        addresses = {i["name"]: i["record"][3] for i in inputs if i["type"] == 19}
        require(addresses == {"global": 0x3ffffffb, "group": 0x3ffffff9, "local": 0x3ffffffa,
                              "flat": 0x3ffffff8}, "Original compute input identities")
        require(observed["global.y"]["status"] == "unavailable" and observed["global.y"]["bits"] is None,
                "Unmapped source component remains unknown")
        require(observed["output"]["status"] == "unsupported_type" and observed["output"]["bits"] is None,
                "Opaque resource remains unknown")
        stores = [i for i in instructions if i[1] == 168]
        require(len(stores) == 1 and stores[0][2] == 1 and scalars[stores[0][10]][1] == 49,
                "Original resource assignment metadata")
    else:
        require(all(i["record"][2] == 0xffffffff and i["record"][3] < 32 for i in inputs),
                "Original ordinary input registers")
    totals = []
    for position, entry in enumerate(observations):
        require(entry["step"] == position, "Complete selected observation sequence")
        for value in entry["values"]:
            if value["name"] == "total" and value["status"] == "available":
                if not totals or totals[-1] != value["value"]:
                    totals.append(value["value"])
    require(totals == (["12", "13", "15"] if stage == "cs" else ["12.5", "13.5", "15.5"]),
            "Authored loop assignment history")
    require(trace[-1]["completed"] is True and trace[-1]["selected_steps"] == len(observations),
            "Complete execution")
    require(trace[0]["shader_sha256"] == model["shader_sha256"], "Trace shader identity")
    require(symbols["shader_stage"] == stage, "Debug stage")
    return {"stage": stage, "selected_steps": len(observations), "total_values": totals,
            "original_inputs": inputs, "source_inputs_verified": len(expected)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    load = lambda name: json.loads((args.directory / name).read_text(encoding="utf-8"))
    results, rejected = [], 0
    for stage in ("vs", "ps", "cs"):
        shader = (args.directory / (stage + ".dxbc")).read_bytes()
        model = load(stage + "-model.json")
        observations = load(stage + "-values.json")
        raw = (args.directory / (stage + ".jsonl")).read_bytes()
        require(hashlib.sha256(raw).hexdigest() == model["trace_sha256"], "Trace digest")
        trace = [json.loads(line) for line in raw.splitlines()]
        results.append(check(stage, shader, model, observations, trace))
        for fault in range(3):
            changed = copy.deepcopy(observations)
            if fault == 0:
                next(v for v in changed[0]["values"] if v["status"] == "available")["bits"] = 0
            elif fault == 1:
                for entry in changed:
                    for value in entry["values"]:
                        if value["name"] == "total" and value["status"] == "available":
                            value["value"] = "0"
            else:
                changed.pop()
            try:
                check(stage, shader, model, changed, trace)
            except ValueError:
                rejected += 1
            else:
                raise ValueError("Corrupted source observation was accepted")
    workflows = []
    for vendor in (0, 4318, 32902):
        folder = args.directory / f"captured-{vendor}"
        worker = folder / "worker"
        result = json.loads((worker / "result.json").read_text(encoding="utf-8"))
        model = json.loads((folder / "model.json").read_text(encoding="utf-8"))
        require(result["ok"] is True and result["context"]["adapter"]["vendor"] == (vendor or 5140)
                and result["context"]["adapter"]["warp"] == (vendor == 0), "Captured adapter identity")
        require(model["watches"] == [{"expression": "total", "status": "available", "type": "uint", "value": "8"}],
                "Captured source watch")
        require(model["model"]["source_variables"]["status"] == "available", "Captured SDBG values")
        for name, info in result["artifacts"].items():
            require(Path(name).name == name, "Artifact path")
            raw = (worker / name).read_bytes()
            require(len(raw) == info["bytes"] and hashlib.sha256(raw).hexdigest() == info["sha256"],
                    "Captured artifact identity")
        rows = [json.loads(line) for line in (worker / "trace.jsonl").read_bytes().splitlines()]
        stores = [c for row in rows if row["record"] == "step" for c in row["buffer_changes"]]
        require(len(stores) == 4 and {c["offset"] for c in stores} == {0, 4, 8, 12}, "Captured group writes")
        original = (folder / "original-output.bin").read_bytes()
        require(original == struct.pack("<4I", 3, 5, 6, 8), "Original GPU arithmetic")
        for change in stores:
            require(change["resource"] == 7 and change["after_valid_hex"] == "01010101" and
                    bytes.fromhex(change["after_hex"]) == original[change["offset"]:change["offset"] + 4],
                    "Original/native exact store comparison")
        modules = [Path(p).name.lower() for p in result["execution"]["loaded_modules"]]
        require(not any("renderdoc" in p or p in ("gpa.dll", "dx11_player.dll", "dx11_playback.dll",
                                                  "shimd3d64.dll") for p in modules), "Native module snapshot")
        workflows.append({"vendor": vendor, "stores": 4, "original_gpu_equal": True,
                          "worker_elapsed_ms": result["execution"]["elapsed_ms"]})
    print(json.dumps({"evidence_valid": True, "full_debugger_parity": False, "workflows": workflows,
                      "cases": results, "corruption_rejections": rejected}, indent=2))


if __name__ == "__main__":
    main()
