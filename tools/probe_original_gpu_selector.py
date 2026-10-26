"""Verify the pinned GPA GPU-ID predicate with owned records, without replay."""
import argparse
import ctypes as c
import json
import os
from pathlib import Path
import subprocess
import sys

from probe_original_debugger_host import PLAYER_SHA256, digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gpa", type=Path, default=Path("C:/Program Files/IntelSWTools/GPA"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.child:
        args.out.mkdir(parents=True, exist_ok=False)
        with (args.out / "process.log").open("w") as log:
            result = subprocess.run([sys.executable, str(Path(__file__).resolve()),
                                     *sys.argv[1:], "--child"], stdout=log, stderr=subprocess.STDOUT,
                                    timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
        if result.returncode:
            raise SystemExit("GPU selector probe failed; inspect process.log")
        return
    player = args.gpa.resolve() / "dx11_player.dll"
    if digest(player) != PLAYER_SHA256 or c.sizeof(c.c_void_p) != 8:
        raise ValueError("Expected pinned x64 player")
    with os.add_dll_directory(str(player.parent)):
        dll = c.WinDLL(str(player))
        predicate = c.WINFUNCTYPE(c.c_ubyte, c.c_void_p, c.c_void_p)(dll._handle + 0x11c100)
        cases = []
        for kind, low, high in ((1, 68403, 0), (1, 73907, 0), (1, 0x12345678, 0xabcdef01), (0, 0, 0)):
            for change in ("none", "kind", "low", "high"):
                record = c.create_string_buffer(0x178)
                c.c_uint32.from_buffer(record, 0x94).value = kind
                c.c_uint32.from_buffer(record, 0x98).value = low
                c.c_uint32.from_buffer(record, 0x9c).value = high
                selection = (c.c_uint32 * 3)(kind, low, high)
                if change != "none":
                    selection[("kind", "low", "high").index(change)] ^= 1
                closure = (c.c_uint64 * 2)(0, c.addressof(selection))
                before = record.raw
                actual = bool(predicate(c.byref(closure), record))
                expected = selection[0] == kind and (kind == 0 or list(selection)[1:] == [low, high])
                row = dict(record_id=[kind, low, high], selection=list(selection), change=change,
                           actual=actual, expected=expected, record_unchanged=record.raw == before)
                cases.append(row)
                if actual != expected or not row["record_unchanged"]:
                    raise ValueError("GPU-ID predicate differs from the disassembled layout")
    report = dict(schema=1, scope="pinned_gpu_id_predicate_owned_record_validation",
                  player_sha256=PLAYER_SHA256, player_unchanged=digest(player) == PLAYER_SHA256,
                  source_sha256=digest(Path(__file__)), engine_recovered=False, cases=cases)
    if not report["player_unchanged"]:
        raise ValueError("Pinned player changed")
    (args.out / "selector.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
