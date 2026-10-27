"""Check a pinned GPA state-block constructor implicated by debugger alias scans.

Research only. Synthetic storage and original code execute in a bounded child;
no replay, shader, debugger engine, product integration or GPU is exercised.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time


PLAYER_SHA256 = '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b'
SIZE, GUARD = 0x490, 0x40
DWORD_FIELDS = (0x38, 0xb8, 0xf0, 0x128, 0x160, 0x198, 0x1d0, 0x208,
                0x240, 0x2d0, 0x330, 0x370, 0x3a8, 0x3e8, 0x428)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, report):
    path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


def expected_storage(before, base, marker, destroyed=False):
    expected = bytearray(before)
    expected[GUARD + 0x10:GUARD + SIZE] = bytes(SIZE - 0x10)
    for field in DWORD_FIELDS:
        expected[GUARD + field + 4:GUARD + field + 8] = before[GUARD + field + 4:GUARD + field + 8]
    struct.pack_into('<Q', expected, GUARD, base + (0x1792c8 if destroyed else 0x179318))
    struct.pack_into('<I', expected, GUARD + 8, marker)
    return bytes(expected)


def child(args):
    binary = args.gpa.resolve() / 'dx11_player.dll'
    if c.sizeof(c.c_void_p) != 8 or digest(binary) != PLAYER_SHA256:
        raise ValueError('Expected the pinned x64 player')
    report = dict(schema=1, scope='original_state_block_synthetic_layout', completed=False,
                  engine_recovered=False, player_sha256=PLAYER_SHA256,
                  source_sha256=digest(Path(__file__)), cases=[])
    output = args.out / 'layout.json'
    write(output, report)
    try:
        with os.add_dll_directory(str(binary.parent)):
            dll = c.WinDLL(str(binary))
            base = dll._handle
            report['module_base'] = base
            table = base + 0x179318
            locator = c.c_uint64.from_address(table - 8).value
            signature, offset, cd_offset, type_rva, hierarchy_rva, self_rva = struct.unpack(
                '<6I', c.string_at(locator, 24))
            if signature != 1 or offset != 0 or cd_offset != 0 or base + self_rva != locator:
                raise ValueError('Unexpected state-block RTTI locator')
            name = c.string_at(base + type_rva + 16, 128).split(b'\0', 1)[0].decode('ascii')
            if name != '.?AVStateBlockD3D11@Playback@@':
                raise ValueError('Unexpected state-block RTTI type')
            report['rtti'] = dict(name=name, locator_rva=self_rva, type_rva=type_rva,
                                  hierarchy_rva=hierarchy_rva, vtable_rva=0x179318)
            constructor = c.WINFUNCTYPE(c.c_void_p, c.c_void_p, c.c_uint32)(base + 0xb2870)
            destructor = c.WINFUNCTYPE(None, c.c_void_p)(base + 0xb3170)
            for pattern in (0xa5, 0x5a):
                for marker in (0, 4, 0x11223344):
                    storage = c.create_string_buffer(bytes([pattern]) * (SIZE + 2 * GUARD), SIZE + 2 * GUARD)
                    address = c.addressof(storage) + GUARD
                    before = storage.raw
                    row = dict(pattern=pattern, marker=marker, before=before.hex(), completed=False)
                    report['cases'].append(row)
                    write(output, report)
                    returned = constructor(address, marker)
                    row['after_constructor'] = storage.raw.hex()
                    row['returned_input'] = returned == address
                    if returned != address or storage.raw != expected_storage(before, base, marker):
                        raise ValueError('Original constructor differs from the recovered guarded layout')
                    destructor(address)
                    row['after_destructor'] = storage.raw.hex()
                    if storage.raw != expected_storage(before, base, marker, True):
                        raise ValueError('Original non-deleting destructor changed unexpected storage')
                    row['completed'] = True
                    write(output, report)
        if digest(binary) != PLAYER_SHA256:
            raise ValueError('Original player changed')
        report['completed'] = True
    finally:
        write(output, report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpa', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--child', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.child:
        child(args)
        return
    args.out.mkdir(parents=True, exist_ok=False)
    command = [sys.executable, str(Path(__file__).resolve()), '--child', '--gpa', str(args.gpa), '--out', str(args.out)]
    start = time.monotonic()
    with (args.out / 'process.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            code = process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            code = None
    write(args.out / 'run.json', dict(command=command, exit_code=code, timed_out=code is None,
          timeout_seconds=30, elapsed_ms=round((time.monotonic() - start) * 1000)))
    if code != 0:
        raise SystemExit('Original layout probe failed; inspect the retained child report and log')
    print('Six guarded original state-block lifecycles completed')


if __name__ == '__main__':
    main()
