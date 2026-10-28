"""Validate saved class-dispatch observations without loading an external analyzer."""
import argparse
import copy
import hashlib
import json
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check(case, old, new, rows):
    vendor = case["vendor_request"]
    require(old["ok"] is True and new["ok"] is True, "Both jobs must have completed")
    require(old["gpa_event"] == new["context"]["options"]["gpa_event"] == 100, "GPA event identity")
    environment = old["replay_environment"]
    require(environment["gpu_vendor_name"] == {0: "Software", 4318: "NVIDIA", 32902: "Intel"}[vendor]
            and environment["degraded"] is False and environment["remote"] is False,
            "Baseline replay environment")
    adapter = new["context"]["adapter"]
    require(adapter["vendor"] == (vendor or 5140) and adapter["warp"] == (vendor == 0), "Native adapter")
    require(new["context"]["frame_sha256"] == case["frame_sha256"], "Native frame identity")
    require(old["step_count"] == 4 and len(old["steps"]) == 4, "Observed baseline steps")
    require([s["nextInstruction"] for s in old["steps"]] == [0, 1, 2, 3], "Observed baseline path")
    require("0: fcall" in old["disassembly"] and "1: store_structured" in old["disassembly"],
            "Baseline instruction correspondence")
    changes = [c["after"] for s in old["steps"] for c in s["changes"] if c["after"]["name"] == "r0"]
    require(len(changes) == 1 and changes[0]["value"]["u32v"][:4] == [0, 0, 0, 0],
            "Observed baseline temporary register")
    require(all(not s["callstack"] for s in old["steps"]), "Observed baseline call stack")
    require(rows[0]["record"] == "header" and rows[-1]["completed"] is True, "Native trace boundaries")
    header = rows[0]
    require(header["frame_sha256"] == case["frame_sha256"] and header["selected_lane"] == 0
            and header["gpa_event"] == 100 and header["gpa_shader"] == 15, "Native trace selection")
    binding = header["class_bindings"]
    require(len(binding) == 1 and binding[0]["object"] == 62, "GPA class identity")
    require(binding[0]["this_bits"] == ([2, 3, 0, 0] if case["created"] else [0, 1, 0, 0])
            and binding[0]["this_valid"] == (15 if case["created"] else 3), "Observed class offsets")
    instructions = {r["index"]: r["dxbc_opcode"] for r in rows if r["record"] == "instruction"}
    steps = [r for r in rows if r["record"] == "step" and r["lane"] == 0]
    require([instructions[r["instruction"]] for r in steps] == [120, 44, 54, 30, 62, 168, 62],
            "Native class body execution")
    require([len(s["return_instruction_stack"]) for s in steps] == [0, 1, 1, 1, 1, 0, 0],
            "Native return boundaries")
    expected = 40 if case["created"] else 20
    for lane in range(4):
        stores = [c for r in rows if r["record"] == "step" and r["lane"] == lane
                  for c in r["buffer_changes"]]
        require(len(stores) == 1 and stores[0]["resource"] == 7 and stores[0]["offset"] == lane * 4
                and stores[0]["after_hex"] == (expected + lane).to_bytes(4, "little").hex()
                and stores[0]["after_valid_hex"] == "01010101", "Authored native group stores")
    modules = [Path(p).name.lower() for p in new["execution"]["loaded_modules"]]
    require(not any("renderdoc" in p or p in ("gpa.dll", "dx11_player.dll", "dx11_playback.dll",
                                              "shimd3d64.dll") for p in modules), "Native module snapshot")
    return {"vendor": vendor, "created": case["created"], "baseline_steps": 4,
            "baseline_r0_at_store": 0, "baseline_class_body_observed": False,
            "native_selected_steps": len(steps), "native_selected_store": expected,
            "native_group_stores": 4}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    root = args.directory
    load = lambda p: json.loads(p.read_text(encoding="utf-8"))
    matrix = load(root / "matrix.json")
    require(matrix["observation_complete"] is True and len(matrix["cases"]) == 6, "Complete observation")
    require({(c["vendor_request"], c["created"]) for c in matrix["cases"]} ==
            {(v, c) for v in (0, 4318, 32902) for c in (False, True)}, "Unique matrix coverage")
    binaries = {Path(f["path"]).name: f["sha256"] for f in matrix["files"]}
    require(binaries["FloraGPA.Rdc.exe"] ==
            "f9c38e18a4e8717f3b37c14a378cd4087baff3c286719b43d6d9f23b98eb9140",
            "Frozen baseline worker identity")
    require(binaries["renderdoc.dll"] ==
            "313f450bfa1f7f7d5f50cc7ee80d9f99b68d3fe873e25eb865ff0433e359f7b3",
            "Pinned migration reference runtime")
    results, rejected = [], 0
    for case in matrix["cases"]:
        label = f'{case["vendor_request"]}-{int(case["created"])}'
        folder = root / (label + "-native-debug")
        old = load(root / (label + "-baseline-debug") / "result.json")
        new = load(folder / "result.json")
        require(all(case[k]["exit_code"] == 0 and case[k]["timeout"] is False
                    for k in ("recapture", "baseline_debug", "native_debug")), "Process outcomes")
        require(new["context"]["backend_sha256"] == binaries["FloraGPA.Worker.exe"], "Native worker identity")
        require(rows_hash := new["data"]["shader_sha256"], "Native shader identity")
        for name, artifact in new["artifacts"].items():
            require(Path(name).name == name, "Artifact filename")
            data = (folder / name).read_bytes()
            require(len(data) == artifact["bytes"] and hashlib.sha256(data).hexdigest() == artifact["sha256"],
                    "Native artifact integrity")
        rows = [json.loads(line) for line in (folder / "trace.jsonl").read_text().splitlines()]
        require(rows[0]["shader_sha256"] == rows_hash == new["artifacts"]["shader.dxbc"]["sha256"],
                "Trace shader identity")
        results.append(check(case, old, new, rows))
        for fault in range(5):
            changed = copy.deepcopy(rows)
            if fault == 0:
                changed[0]["class_bindings"][0]["object"] = 63
            elif fault == 1:
                changed[0]["frame_sha256"] = "wrong"
            elif fault == 2:
                changed[0]["class_bindings"][0]["this_valid"] = 0
            elif fault == 3:
                next(r for r in changed if r["record"] == "step" and r["buffer_changes"])[
                    "buffer_changes"][0]["after_hex"] = "00000000"
            else:
                next(r for r in changed if r["record"] == "step" and r["return_instruction_stack"])[
                    "return_instruction_stack"] = []
            try:
                check(case, old, new, changed)
            except ValueError:
                rejected += 1
            else:
                raise ValueError("Corrupted trace was accepted")
    print(json.dumps({"evidence_valid": True, "full_parity": False, "cases": results,
                      "corruption_rejections": rejected}, indent=2))


if __name__ == "__main__":
    main()
