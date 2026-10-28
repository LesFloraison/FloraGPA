"""Probe pinned GPA host accessors in a bounded, isolated child process.

Research only. No host constructor, replay, GPU, query manager, power-setting
function or debugger request runs. A child-local singleton is restored before
exit. Synthetic nonzero fields are markers, not recovered engine objects.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


SHA = '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b'
CODE = {0x8ab90: '8891f8000000c3',
        0x89420: '488b0519f11700c3',
        0x87bd0: '488b05690918004885c07405803800750233c0c3',
        0x87bf0: '488b05490918004885c0740b80b898010000000f95c0c3c3'}
SIZE, GUARD = 0x3d0, 0x40


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def child(args):
    binary = args.gpa / 'dx11_player.dll'
    if c.sizeof(c.c_void_p) != 8 or digest(binary) != SHA:
        raise ValueError('Expected the pinned x64 player')
    report = dict(schema=1, scope='original_host_synthetic_accessors', completed=False,
                  engine_recovered=False, binary=str(binary), player_sha256=SHA,
                  source_sha256=digest(Path(__file__)), code={}, cases=[])
    output = args.out / 'accessors.json'
    write(output, report)
    with os.add_dll_directory(str(args.gpa)):
        library = c.WinDLL(str(binary))
        base = library._handle
        for rva, encoded in CODE.items():
            actual = c.string_at(base + rva, len(bytes.fromhex(encoded))).hex()
            if actual != encoded:
                raise ValueError(f'Loaded accessor code changed at {rva:#x}')
            report['code'][hex(rva)] = actual
        singleton = c.c_void_p.from_address(base + 0x208540)
        saved = singleton.value
        # No initialized host is permitted in this fresh child.
        if saved is not None:
            raise ValueError('Unexpected existing original host')
        raw = c.WINFUNCTYPE(c.c_void_p)(base + 0x89420)
        active = c.WINFUNCTYPE(c.c_void_p)(base + 0x87bd0)
        flag = c.WINFUNCTYPE(c.c_ubyte)(base + 0x87bf0)
        setter = c.WINFUNCTYPE(None, c.c_void_p, c.c_ubyte)(base + 0x8ab90)
        report['null'] = dict(raw=raw(), active=active(), flag=flag())
        if report['null'] != dict(raw=None, active=None, flag=0):
            raise ValueError('Unexpected null singleton result')
        try:
            for pattern in (0xa5, 0x5a):
                for enabled in (0, 1, 0xff):
                    for stored_flag in (0, 1, 0x80):
                        for value in (0, 1, 0xff):
                            seed = bytearray([pattern] * (SIZE + 2 * GUARD))
                            seed[GUARD] = enabled
                            seed[GUARD + 0x198] = stored_flag
                            storage = c.create_string_buffer(bytes(seed), len(seed))
                            host = c.addressof(storage) + GUARD
                            singleton.value = host
                            returned = dict(raw=raw(), active=active(), flag=flag())
                            if storage.raw != bytes(seed) or singleton.value != host:
                                raise ValueError('Accessor changed guarded host storage or singleton')
                            setter(host, value)
                            expected = seed[:]
                            expected[GUARD + 0xf8] = value
                            if (storage.raw != bytes(expected) or singleton.value != host or
                                    returned != dict(raw=host, active=host if enabled else None,
                                                     flag=int(stored_flag != 0))):
                                raise ValueError('Unexpected accessor result or setter write')
                            report['cases'].append(dict(pattern=pattern, enabled=enabled,
                                stored_flag=stored_flag, value=value, host=host,
                                returned=returned, before=bytes(seed).hex(), after=storage.raw.hex()))
                            singleton.value = None
        finally:
            singleton.value = saved
            report['singleton_restored'] = singleton.value == saved
            write(output, report)
    if digest(binary) != SHA:
        raise ValueError('Installed player changed')
    report['completed'] = True
    write(output, report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpa', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--child', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.gpa = args.gpa.resolve()
    args.out = args.out.resolve()
    if args.child:
        child(args)
        return
    args.out.mkdir(parents=True, exist_ok=False)
    command = [sys.executable, str(Path(__file__).resolve()), '--gpa', str(args.gpa),
               '--out', str(args.out), '--child']
    run = dict(command=command, timeout_seconds=30, timed_out=False, completed=False)
    start = time.monotonic()
    try:
        with (args.out / 'child.log').open('wb') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=30)
        run.update(exit_code=result.returncode, completed=result.returncode == 0)
    except subprocess.TimeoutExpired:
        run['timed_out'] = True
    finally:
        run['elapsed_ms'] = round((time.monotonic() - start) * 1000)
        write(args.out / 'run.json', run)
    if not run['completed']:
        raise SystemExit('Original host accessor probe did not complete')
    print(json.dumps(run))


if __name__ == '__main__':
    main()
