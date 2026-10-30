"""Validate native minimum-precision texture loads against direct GPU shaders."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path
import re
import struct


def require(value, message):
    if not value:
        raise ValueError(message)


def instructions(encoded):
    data = bytes.fromhex(encoded)
    require(data[:4] == b'DXBC' and len(data) >= 32, 'DXBC identity')
    length, count = struct.unpack_from('<II', data, 24)
    require(length == len(data) and 0 < count < 32, 'DXBC container bounds')
    parts = []
    for i in range(count):
        offset, = struct.unpack_from('<I', data, 32 + 4 * i)
        size, = struct.unpack_from('<I', data, offset + 4)
        require(offset + 8 + size <= len(data), 'DXBC chunk bounds')
        if data[offset:offset+4] in (b'SHEX', b'SHDR'):
            require(size % 4 == 0, 'DXBC word alignment')
            parts.append(struct.unpack_from('<' + 'I' * (size // 4), data, offset + 8))
    require(len(parts) == 1, 'Unique shader program')
    words, = parts
    require(words[1] == len(words), 'DXBC word count')
    rows, position = [], 2
    while position < len(words):
        size = (words[position] >> 24) & 127
        require(size and position + size <= len(words), 'DXBC instruction bounds')
        rows.append(words[position:position+size]); position += size
    return words[0], rows


def validate(report, backend="dxbc-gpu/22", allow_uav=False):
    require(report['schema'] == 1 and report['probe'] == 'minimum_precision_texture_load' and
            report['passed'], 'Probe scope')
    vendor, dimension, stage, precision = (report[k] for k in ('vendor', 'dimension', 'stage', 'precision'))
    require(vendor in (0, 4318, 32902) and dimension in (2, 3, 5, 7, 8) and
            stage in (0, 1, 5) and precision in (1, 2, 4, 5), 'Owned fixture identity')
    require(report['format'] == (2 if precision < 4 else 4 if precision == 4 else 3), 'Resource format')
    uav = report.get('uav', False)
    require(type(uav) is bool and (not uav or allow_uav and stage in (0, 5)), 'Texture binding scope')
    helper = report['helper']
    require(helper['backend'] == backend and helper['warp'] == (vendor == 0) and
            helper['vendor_id'] == (5140 if vendor == 0 else vendor) and
            helper['completion_wait'] in ('fence-event/1', 'native-poll/1'), 'Selected helper identity')
    expected_cases = set(itertools.product((0, 4, 5), range(4), (False,) if stage == 5 else (False, True),
                                           (False, True)))
    records = report['records']
    require(len(records) == len(expected_cases) and
            {(r['address_precision'], r['mode'], r['output_destination'], r['literal_address']) for r in records} == expected_cases,
            'Complete address/output matrix')
    for row in records:
        mode, ap, direct = row['mode'], row['address_precision'], row['output_destination']
        mask = 9 if mode & 1 else 15
        require(row['mask'] == mask and row['address'] == [65537 if mode == 1 else 0xffffffff if mode == 3 else 1,
                1, 1, 1 if mode == 2 else 0], 'Original coordinate selection')
        require(len(row['expected']) == 4 and all(type(v) is int and 0 <= v <= 0xffffffff for v in row['expected']) and
                row['actual'] == row['expected'] and row['valid'] == 15, 'GPU register result')
        zero = all(row['expected'][c] == 0 for c in range(4) if mask & (1 << c))
        require(row['unknown_valid'] == (15 if zero else 15 ^ mask), 'Unknown texel versus out-of-bounds validity')
        if ap == 0:
            require(zero == (mode in (1, 3) or mode == 2 and dimension == 5), 'Full-width coordinate bounds')
        if uav:
            require(row['cancellation_checked'] is (mode == 0 and not direct and not row['literal_address']),
                    'Cancellation and recovery coverage')
            native = {2: 2, 3: 4, 5: 8, 7: 3, 8: 5}[dimension]
            mip = int(mode == 2)
            count = 2 if dimension in (7, 8) else (2 >> mip) if dimension == 5 else 0
            require(row['view_descriptor'] == [report['format'], native, mip, 0, count], 'UAV subresource view')
        header, rows = instructions(row['code'])
        require(header == (stage << 16 | 0x50), 'Original shader stage')
        require(sum((r[0] & 2047) == 106 and bool(r[0] & (1 << 16)) for r in rows) == 1,
                'Minimum-precision declaration')
        loads = [r for r in rows if r[0] & 2047 == (163 if uav else 45)]
        require(len(loads) == 1, 'Original load count')
        load, = loads
        require(load[0] & 0x80000000 and load[1] == 2 | dimension << 6 | 0x80000000 and
                load[2] == 3 | ((5 if precision < 4 else 3 if precision == 4 else 4) * 0x1111 << 6),
                'Original typed resource extensions')
        position = 3
        literal = row['literal_address']
        for kind, indices, expected_precision in ((2 if direct else 0, [0 if direct else 3], precision),
                                                  (4 if literal else 8, [] if literal else [2, 0], ap),
                                                  (30 if uav else 7, [1] if uav else [3], 0)):
            token = load[position]; position += 1
            require((token >> 12 & 255) == kind and (token >> 20 & 3) == len(indices), 'Original operand identity')
            actual_precision = 0
            if token & 0x80000000:
                extension = load[position]; position += 1
                require(extension & 63 == 1 and not extension & 0x80000000, 'Precision extension')
                actual_precision = extension >> 14 & 7
            require(actual_precision == expected_precision and list(load[position:position+len(indices)]) == indices,
                    'Original operand precision and indices')
            if kind in (0, 2):
                require(token >> 4 & 15 == mask, 'Original destination mask')
            position += len(indices)
            if kind == 4:
                require(list(load[position:position+4]) == row['address'], 'Original immediate coordinates')
                position += 4
        require(position == len(load), 'Original load tail')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--test-log', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--backend', default='dxbc-gpu/22')
    parser.add_argument('--texture-uav', action='store_true', help='Require PS/CS texture UAV precision coverage')
    parser.add_argument('--compare-srv', type=Path, help='Require every historical SRV record to remain identical')
    args = parser.parse_args()
    files, records, seen, controls = [], 0, set(), []
    historical_files, unchanged_records = [], 0
    identity = lambda p: dict(path=p.resolve().as_posix(), bytes=p.stat().st_size,
                             sha256=hashlib.sha256(p.read_bytes()).hexdigest())
    for file in sorted(args.root.glob('*.json')):
        report = json.loads(file.read_text())
        validate(report, args.backend, args.texture_uav)
        key = tuple(report[k] for k in ('vendor', 'dimension', 'stage', 'precision')) + (report.get('uav', False),)
        require(key not in seen, 'Duplicate fixture')
        seen.add(key); files.append(identity(file)); records += len(report['records'])
        if args.compare_srv and not report.get('uav', False):
            historical = args.compare_srv / file.name
            before = json.loads(historical.read_text(encoding='utf-8'))
            require(len(before['records']) == len(report['records']), 'Historical SRV matrix size')
            for old, new in zip(before['records'], report['records']):
                require(all(new[k] == value for k, value in old.items()), 'Unchanged historical SRV record')
                unchanged_records += 1
            historical_files.append(identity(historical))
        if report['dimension'] == 3 and report['precision'] == 1:
            mutations = ['bits', 'validity', 'unknown', 'backend', 'missing', 'code']
            if report.get('uav', False):
                mutations += ['view', 'cancellation']
            for kind in mutations:
                changed = copy.deepcopy(report)
                if kind == 'bits': changed['records'][0]['actual'][0] ^= 1
                elif kind == 'validity': changed['records'][0]['valid'] = 0
                elif kind == 'unknown': changed['records'][0]['unknown_valid'] ^= 1
                elif kind == 'backend': changed['helper']['backend'] = 'dxbc-gpu/21'
                elif kind == 'missing': changed['records'].pop()
                elif kind == 'view': changed['records'][0]['view_descriptor'][2] = 99
                elif kind == 'cancellation': changed['records'][0]['cancellation_checked'] = False
                else: changed['records'][0]['code'] = '00' + changed['records'][0]['code'][2:]
                try: validate(changed, args.backend, args.texture_uav)
                except ValueError as error: controls.append(dict(case=key, change=kind, reason=str(error)))
                else: raise ValueError('Corrupted precision evidence accepted')
    expected = set(itertools.product((0, 4318, 32902), (2, 3, 5, 7, 8), (0, 1, 5), (1, 2, 4, 5), (False,)))
    if args.texture_uav:
        expected |= set(itertools.product((0, 4318, 32902), (2, 3, 5, 7, 8), (0, 5), (1, 2, 4, 5), (True,)))
    require(seen == expected, 'Complete device/dimension/stage/precision matrix')
    log = args.test_log.read_text(encoding='utf-8-sig')
    require(len(re.findall(r'PASS\s+: DxbcTextureLoadTests::minimumPrecision\(', log)) == len(expected) and
            re.search(r'Totals: \d+ passed, 0 failed, 0 skipped, 0 blacklisted,', log), 'Precision test completion')
    root = Path(__file__).resolve().parents[1]
    sources = ['src/application/DxbcExecution.cpp', 'src/application/DxbcTextureInputs.cpp',
               'src/application/DxbcTextureInputs.h', 'src/application/DxbcTextureUav.cpp', 'tests/TextureOperationOracle.h', 'src/application/DxbcGpuOperations.cpp',
               'src/application/DxbcGpuOperations.h', 'tests/DxbcTextureLoadTests.cpp',
               'tools/validate_texture_precision.py']
    ledger = dict(schema=1, scope='native_minimum_precision_texture_load', passed=True, fixtures=len(seen),
                  direct_gpu_comparisons=records, full_debugger_parity=False, renderdoc_removed=False,
                  negative_controls=controls, files=files+[identity(args.test_log)],
                  unchanged_srv_records=unchanged_records, historical_files=historical_files,
                  sources=[identity(root / s) for s in sources])
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(ledger, stream, indent=2); stream.write('\n')
    print(f'Validated {len(seen)} fixtures, {records} GPU comparisons and {len(controls)} corruptions')


if __name__ == '__main__':
    main()
