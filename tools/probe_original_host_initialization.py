"""Bounded original-code alias probes and serial GF2 host-graph observations.

Research only: this loads the hash-pinned local GPA player in child processes.
It does not install an engine, classify shader stages, or claim shader execution.
"""
import argparse
import ctypes as c
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

from probe_original_debugger_host import PLAYER_SHA256, P, U, Wide, digest


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def synthetic(args):
    player = args.gpa.resolve() / "dx11_player.dll"
    if digest(player) != PLAYER_SHA256 or c.sizeof(P) != 8:
        raise ValueError("Expected the pinned x64 player")
    rows = []
    with os.add_dll_directory(str(player.parent)):
        dll = c.WinDLL(str(player))
        base = dll._handle
        setting = c.WINFUNCTYPE(None, P, P)(base + 0x895b0)

        def set_value(key, value):
            a, b = Wide(key), Wide(value)
            setting(a.ptr, b.ptr)

        set_value("single-threaded", "true")
        if c.c_ubyte.from_address(base + 0x20851b).value != 1:
            raise ValueError("Refusing to start a thread against synthetic storage")
        for no_first in (False, True):
            set_value("no-first-playbacks", "true" if no_first else "false")
            flag = c.c_ubyte.from_address(base + 0x20851c).value
            if bool(flag) != no_first:
                raise ValueError("Unexpected original initialization flag")
            for rva, subobject, destination in ((0xa0130, 8, 0x18), (0x9bab0, 0x48, 0x48),
                                               (0x8e510, 0xf8, 0x190)):
                # Guards and unrelated host bytes must retain their original pattern.
                storage = c.create_string_buffer(b"\xa5" * 0x440, 0x440)
                host = c.addressof(storage) + 0x20
                before = storage.raw
                expected = bytearray(before)
                struct.pack_into("<Q", expected, 0x20 + destination, host)
                if rva == 0x8e510:
                    expected[0x20 + 0x199] = int(not no_first)
                    expected[0x20 + 0x19a] = 0
                c.WINFUNCTYPE(None, P, P)(base + rva)(host + subobject, host)
                after = storage.raw
                if after != expected:
                    raise ValueError(f"Unexpected alias initialization writes at {rva:#x}")
                rows.append(dict(rva=hex(rva), subobject=hex(subobject), destination=hex(destination),
                                 no_first_playbacks=no_first, host=hex(host),
                                 before=before.hex(), after=after.hex(), unchanged_engine=True))
    if digest(player) != PLAYER_SHA256:
        raise ValueError("Original player changed")
    write(args.out / "synthetic.json", dict(completed=True, player_sha256=PLAYER_SHA256,
          source_sha256=digest(Path(__file__)), rows=rows, engine_recovered=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path)
    parser.add_argument("--gpa", type=Path, default=Path("C:/Program Files/IntelSWTools/GPA"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--synthetic-child", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.synthetic_child:
        synthetic(args)
        return
    if not args.capture or not args.capture.is_file():
        parser.error("Provide the pinned GF2 capture for event 113")
    if digest(args.capture) != "fc7b7f7de20a578da2977b2ff91d96f5cf80887b393bc85859be8cab1af91c8e":
        parser.error("Capture does not match the researched GF2 identity")
    args.out.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parent
    manifest = dict(completed=False, capture_sha256=digest(args.capture), runs=[],
                    sources=[dict(path=str(p), sha256=digest(p)) for p in
                             (Path(__file__).resolve(), root / "probe_original_debugger_host.py")])

    def run(name, command, deadline):
        target = args.out / name
        target.mkdir()
        command += ["--gpa", str(args.gpa), "--out", str(target)]
        start = time.monotonic()
        with (target / "process.log").open("w", encoding="utf-8") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                code = process.wait(timeout=deadline)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                code = None
        manifest["runs"].append(dict(name=name, command=command, exit_code=code,
            timed_out=code is None, timeout_seconds=deadline, elapsed_ms=round((time.monotonic()-start)*1000)))
        write(args.out / "runs.json", manifest)
        if code != 0:
            raise SystemExit(f"Probe failed: {name}; inspect retained process and partial reports")

    run("synthetic", [sys.executable, str(Path(__file__).resolve()), "--synthetic-child"], 30)
    for vendor, label in ((4318, "nvidia"), (32902, "intel")):
        for defaults in (False, True):
            command = [sys.executable, str(root / "probe_original_debugger_host.py"), str(args.capture),
                       "--event", "113", "--vendor", str(vendor), "--host-graph", "--child"]
            if defaults:
                command.append("--original-initialization")
            run(f"gf2-{label}-{'default' if defaults else 'controlled'}", command, 120)
    if digest(args.capture) != manifest["capture_sha256"]:
        raise ValueError("Capture changed")
    manifest["completed"] = True
    write(args.out / "runs.json", manifest)
    print(str(args.out / "runs.json"))


if __name__ == "__main__":
    main()
