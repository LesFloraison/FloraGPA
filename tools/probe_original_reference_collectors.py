"""Research-only original fixed ERG collectors on complete, bounded records.

No replay, fake COM objects, malformed native input, or persistent patches. The
hash-pinned factory allocates each ERG in this isolated process. Its allocations
are deliberately retained until process exit to avoid guessing shared ownership.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import sys

PLAYER = '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b'
FIXED_COLLECTORS = {0x23be0, 0x23da0, 0x23e20, 0x23ec0, 0x23f60, 0x24160, 0x241e0, 0x24400}


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--case', nargs=2, action='append', required=True, metavar=('CAPTURE', 'COMMANDS'))
    parser.add_argument('--reference-python', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.reference_python.resolve(strict=True)))
    from frame import Frame
    path = Path('C:/Program Files/IntelSWTools/GPA/dx11_player.dll')
    if hashlib.sha256(path.read_bytes()).hexdigest() != PLAYER:
        raise ValueError('Unknown player')
    report = dict(schema='FloraGPA fixed original collector probe 1', completed=False, passed=False,
                  player_sha256=PLAYER, gpu_execution=False, cases=[], sources=[],
                  conditional_types_excluded=[0x25e],
                  source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    def save():
        (args.out/'collectors.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    save()
    samples = {}
    for capture, commands in args.case:
        frame = Frame(capture)
        report['sources'].append(dict(capture=capture, commands=commands,
            capture_sha256=hashlib.sha256(Path(capture).read_bytes()).hexdigest(),
            commands_sha256=hashlib.sha256(Path(commands).read_bytes()).hexdigest()))
        for row in json.loads(Path(commands).read_text(encoding='utf-8'))['commands']:
            info = row['original_initialization']
            if info['status'] == 'recovered' and row['type'] != 0x25e and row['type'] not in samples:
                raw = bytes(frame.payload(row['id']))
                if row['status'] != 'decoded' or len(raw) != row['wire_size'] or raw != b''.join(
                        bytes.fromhex(f['hex']) for f in row['fields']):
                    raise ValueError('Command export does not match the complete saved record')
                samples[row['type']] = (row, raw)
    if not samples or len(samples) > 32:
        raise ValueError('Unexpected sample count')
    rng = random.Random(0x23be0)
    with os.add_dll_directory(str(path.parent)):
        dll = c.WinDLL(str(path))
        base = dll._handle
        u64, u32, ptr = c.c_uint64, c.c_uint32, c.c_void_p
        factory = c.WINFUNCTYPE(ptr, ptr, ptr, u32, c.c_uint16)(base+0x3c550)
        for wire_type, (row, original) in sorted(samples.items()):
            info = row['original_initialization']
            rva = int(info['collector_rva'], 16)
            if rva not in FIXED_COLLECTORS:
                raise ValueError('Collector requires unproven native dependencies')
            for mode in ['captured', 'zeros', 'high_zero', 'all_ones', 'duplicates', 'random']:
                raw = bytearray(original)
                values = {}
                for field in row['fields']:
                    if field['encoding'] != 'Q':
                        continue
                    value = dict(captured=field['value'], zeros=0, high_zero=1<<32,
                                 all_ones=(1<<64)-1, duplicates=7, random=rng.getrandbits(64))[mode]
                    struct.pack_into('<Q', raw, field['offset'], value)
                    values[field['name']] = value
                names = [r['field'] for r in info['references']]
                if wire_type == 0x246 and 'data' not in names:
                    names.append('data')
                expected = [values[n] & 0xffffffff for n in names
                            if not (wire_type == 0x246 and n == 'data' and values[n] == 0)]
                shared = (u64*2)()
                factory(None, shared, row['id'], wire_type)
                obj = shared[0]
                if not obj:
                    raise ValueError('Original factory returned no ERG')
                vt = u64.from_address(obj).value
                actual_rva = u64.from_address(vt+4*8).value-base
                if actual_rva != rva:
                    raise ValueError('Different original collector')
                backing = c.create_string_buffer(bytes(raw))
                start = c.addressof(backing)
                vec = (u64*3)(start, start+len(raw), start+len(raw))
                ref = (u64*2)(c.addressof(vec), 0)
                output = (u32*128)()
                address = c.addressof(output)
                dest = (u64*3)(address, address, address+c.sizeof(output))
                c.WINFUNCTYPE(None,ptr,ptr,ptr)(base+rva)(obj,ref,dest)
                if dest[0] != address or not address <= dest[1] <= dest[2] or dest[2] != address+c.sizeof(output):
                    raise ValueError('Unexpected native output allocation')
                actual = list(output[:(dest[1]-address)//4])
                report['cases'].append(dict(type=wire_type,event=row['id'],mode=mode,
                    collector_rva=hex(rva),raw_hex=raw.hex(),expected=expected,actual=actual,
                    passed=actual==expected))
                save()
                if actual != expected:
                    raise ValueError(f'Collector mismatch: {wire_type:x}/{mode}')
    report.update(completed=True,passed=True,wire_types=len(samples),checks=len(report['cases']))
    save()
    print(json.dumps(dict(passed=True,wire_types=len(samples),checks=len(report['cases']))))


if __name__ == '__main__':
    main()
