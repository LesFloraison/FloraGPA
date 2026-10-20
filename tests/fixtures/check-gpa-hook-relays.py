"""Offline validation of the observed GPA/RenderDoc GetProcAddress hook fault.

Consumes the bounded debugger report, without starting GPA or a GPU replay.
Checks the two rel32 overflows and their recorded destinations. This is research
evidence for a loader conflict, not verification of any rendered frame.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def require(condition, message):
    if not condition:
        raise ValueError(message)


def address(observation):
    text = observation["address"]
    require(isinstance(text, str) and text.startswith("0x"), "Address must be a hex string")
    value = int(text, 16)
    require(0 <= value < 2**64, "Address exceeds uint64")
    return value


def relative_target(instruction, encoded, offset=0):
    require(encoded[offset] == 0xE9, "Expected an x64 rel32 jump")
    return instruction + offset + 5 + struct.unpack_from("<i", encoded, offset + 1)[0]


def analyze(path):
    raw = path.read_bytes()
    report = json.loads(raw)
    before = report["pre_resume_get_proc_address"]
    after = report["fault_get_proc_address"]
    entry = after["gpa_import"]
    source = address(entry)
    require(source == address(before["gpa_import"]), "Import target changed")
    require(Path(entry["module"]["path"]).name.lower() == "renderdoc.dll",
            "Expected the RenderDoc hook as the GPA import target")
    require(entry["module"] == before["gpa_import"]["module"], "Hook module identity changed")
    require(before["kernel32_export"] == after["kernel32_export"],
            "Kernel32 export observation changed")

    jump = relative_target(source, bytes.fromhex(entry["bytes"]))
    missing = after["gpa_import_jump"]
    require(jump == address(missing) and missing["memory"]["state"] == 0x10000,
            "Encoded entry jump does not target the recorded free memory")
    relay = after["gpa_import_jump_plus_4g"]
    relay_address = address(relay)
    require(relay_address == jump + 2**32, "Relay must be exactly 4 GiB above the bad target")
    require(relay["memory"]["state"] == 0x1000 and relay["memory"]["type"] == 0x20000
            and relay["memory"]["protect"] & 0xF0, "Relay is not committed executable private memory")
    relay_bytes = bytes.fromhex(relay["bytes"])
    require(relay_bytes[:6] == b"\xff\x25\0\0\0\0", "Expected the absolute GPA detour relay")
    detour = struct.unpack_from("<Q", relay_bytes, 6)[0]
    owners = [module for module in report["target_modules"]
              if int(module["base"]) <= detour < int(module["base"]) + module["size"]]
    require(len(owners) == 1 and Path(owners[0]["path"]).name.lower() == "gpa-player.exe",
            "Relay destination is not inside the recorded GPA player")
    require(detour - int(owners[0]["base"]) == 0x7F3F40, "Unexpected GPA GetProcAddress detour RVA")

    original = after["gpa_saved_original"]
    trampoline = address(original)
    saved = bytes.fromhex(original["bytes"])
    prologue = bytes.fromhex(before["gpa_import"]["bytes"])[:11]
    require(prologue == bytes.fromhex("415441574881ec98000000") and saved[:11] == prologue,
            "Original instructions do not match the verified 11-byte prologue")
    require(trampoline + 32 == relay_address, "Unexpected trampoline/relay slot layout")
    broken_return = relative_target(trampoline, saved, 11)
    correct_return = source + 11
    require(broken_return == correct_return + 2**32, "Saved-prologue return is not the observed overflow")
    faults = [fault for fault in report["exceptions"]
              if fault["code"] == "0xc0000005"
              and address(fault["instruction"]) == jump
              and fault["parameters"] == ["0x8", hex(jump)]]
    require(faults, "No execution access violation at the encoded entry target")
    return {
        "status": "observed_entry_and_return_rel32_overflow",
        "scope": "Instruction/address evidence only; no frame or original-GPA fidelity acceptance",
        "report_sha256": hashlib.sha256(raw).hexdigest(),
        "hook": hex(source), "bad_entry_target": hex(jump),
        "existing_relay": hex(relay_address), "gpa_detour_rva": "0x7f3f40",
        "saved_prologue": hex(trampoline), "bad_return": hex(broken_return),
        "correct_return": hex(correct_return), "wrap_bytes": str(2**32),
        "recorded_repair": report.get("relay_repair"),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        result = analyze(args.report)
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2)
            stream.write("\n")
    except (ValueError, KeyError, IndexError, TypeError, OSError, struct.error) as error:
        parser.exit(1, f"Hook evidence rejected: {error}\n")
