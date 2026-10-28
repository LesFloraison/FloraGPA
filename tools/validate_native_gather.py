"""Check native gather evidence against independently submitted original shaders."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path
import re
import struct
from validate_texture_precision import instructions, require


def validate(report):
    require(report['schema'] == 1 and report['probe'] == 'native_texture_gather' and report['passed'],
            'Gather report scope')
    vendor, stage, dimension, opcode, fmt = (report[k] for k in ('vendor', 'stage', 'dimension', 'opcode', 'format'))
    comparison = opcode in (126, 128)
    programmable = opcode in (127, 128)
    require(vendor in (0, 4318, 32902) and stage in (0, 1, 5) and dimension in (3, 6, 8, 10) and
            opcode in (109, 126, 127, 128) and (not programmable or dimension in (3, 8)) and
            fmt in ((41,) if comparison else (2, 3, 4, 28, 31)), 'Gather configuration')
    helper = report['helper']
    require(helper['backend'] == 'dxbc-gpu/23' and helper['warp'] == (vendor == 0) and
            helper['vendor_id'] == (5140 if vendor == 0 else vendor) and
            helper['completion_wait'] in ('fence-event/1', 'native-poll/1'), 'Explicit helper identity')
    records = report['records']
    unsupported = fmt in (3, 4)
    expected_cases = {(0, 0)} if unsupported else set(itertools.product(range(4), range(1 if comparison else 4)))
    require(len(records) == len(expected_cases) and
            {(r['mode'], r['component']) for r in records} == expected_cases, 'Complete gather matrix')
    returns = (5 if fmt in (2, 28, 31, 41) else 4 if fmt == 3 else 3) * 0x1111
    for row in records:
        mode, component = row['mode'], row['component']
        mask = 9 if mode & 1 else 15
        if unsupported:
            require(row['supported'] is False and row['diagnostic_id'] == 416 and
                    row['diagnostic_opcode'] == 109 and
                    not row['format_support'] & 0x800000 and "does not support 'Gather'" in row['diagnostic'] and
                    row['helper_error'] == 'Selected device does not support sampling this texture format',
                    'Explicit unsupported format contract')
            control_header, control_rows = instructions(row['diagnostic_code'])
            require(control_header == stage << 16 | 0x50 and
                    sum((r[0] & 2047) == 109 for r in control_rows) == 1 and
                    all((r[0] & 2047) not in (126, 127, 128) for r in control_rows),
                    'Independent ordinary gather format control')
        else:
            require(row['trace_roundtrip'] == (mode == 0 and component == 0 and dimension == 3 and fmt in (2, 41)),
                    'Gather trace roundtrip selection')
            coordinates = ([1, 1 if mode == 1 else .25, 1 if mode == 1 else -.5, 0]
                           if dimension in (6, 10) else [-.0625 if mode == 1 else .4375, .5625, 0, 0])
            if mode == 0 and dimension in (8, 10): coordinates[2 if dimension == 8 else 3] = 1
            require(row['address'] == list(struct.unpack('<4I', struct.pack('<4f', *coordinates))),
                    'Original coordinates and relative array selection')
            require(row['mask'] == mask and row['valid'] == 15 and row['unknown_valid'] == 15 ^ mask,
                    'Gather validity and partial writes')
            require(len(row['expected']) == 4 and row['expected'] == row['actual'] and
                    all(type(v) is int and 0 <= v <= 0xffffffff for v in row['actual']), 'Direct GPU result')
            for c, seed in enumerate((17, 23, 31, 47)):
                if not mask & (1 << c): require(row['actual'][c] == seed, 'Untouched component')
            require(row['first_mip'] == (1 if mode == 2 else 0) and
                    row['first_layer'] == (1 if dimension == 8 else 6 if dimension == 10 else 0) * (mode == 2)
                    and row['resource_min_lod'] == (.75 if mode == 3 else 0), 'View and resource selection')
            require(row['offset'] == [0xffffffdf if mode == 1 else 64 if mode == 3 else 31,
                                      65 if mode == 1 else 0xffffffe0, 17, 23] and
                    row['reference'] == [0x3f000000, 0, 0, 0], 'Original programmable inputs')
        header, rows = instructions(row['code'])
        require(header == stage << 16 | 0x50, 'Original stage')
        selected = [r for r in rows if r[0] & 2047 in (109, 126, 127, 128)]
        require(len(selected) == 1, 'One original gather')
        instruction, = selected
        require(instruction[0] & 2047 == opcode and instruction[0] & 0x80000000, 'Original opcode')
        immediate = not programmable and dimension in (3, 8) and mode == 1
        require(instruction[1] == 0x80000001 | (((15 << 9) | (1 << 13)) if immediate else 0) and
                instruction[2] == 0x80000002 | dimension << 6 and
                instruction[3] == 3 | returns << 6, 'Original instruction extensions')
        expected = [0x100002 | mask << 4, 3, 0x208e46, 2, 0]
        if programmable: expected += [0x208e46, 2, 1]
        expected += [0x1071b6, 3, 0x10600a | component << 4, 4]
        if comparison: expected += [0x20800a, 2, 2]
        require(list(instruction[4:]) == expected, 'Original operand identities and selections')
        require(any(r[0] & 2047 == 88 and r[0] >> 11 & 31 == dimension and
                    list(r[1:]) == [0x1070f2, 3, returns] for r in rows), 'Original texture declaration')
        require(any(r[0] & 2047 == 90 and r[0] >> 11 & 15 == int(comparison) and
                    list(r[1:]) == [0x106000, 4] for r in rows), 'Original sampler declaration')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--test-log', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    identity = lambda p: dict(path=p.resolve().as_posix(), bytes=p.stat().st_size,
                             sha256=hashlib.sha256(p.read_bytes()).hexdigest())
    expected = set()
    for vendor, stage, dimension, opcode in itertools.product((0, 4318, 32902), (0, 1, 5), (3, 6, 8, 10), (109, 126, 127, 128)):
        if opcode in (127, 128) and dimension in (6, 10): continue
        for fmt in ((41,) if opcode in (126, 128) else (2, 3, 4, 28, 31)):
            expected.add((vendor, stage, dimension, opcode, fmt))
    seen, files, controls, count, rejected = set(), [], [], 0, 0
    for file in sorted(args.root.glob('*.json')):
        report = json.loads(file.read_text())
        validate(report)
        key = tuple(report[k] for k in ('vendor', 'stage', 'dimension', 'opcode', 'format'))
        require(key not in seen, 'Unique fixture')
        seen.add(key); files.append(identity(file))
        if report['format'] in (3, 4): rejected += 1
        else: count += len(report['records'])
        if report['dimension'] == 3 and report['opcode'] == 128:
            for kind in ('bits', 'validity', 'unknown', 'backend', 'missing', 'opcode', 'component', 'code'):
                changed = copy.deepcopy(report)
                if kind == 'bits': changed['records'][0]['actual'][0] ^= 1
                elif kind == 'validity': changed['records'][0]['valid'] = 0
                elif kind == 'unknown': changed['records'][0]['unknown_valid'] ^= 1
                elif kind == 'backend': changed['helper']['backend'] = 'dxbc-gpu/22'
                elif kind == 'missing': changed['records'].pop()
                elif kind == 'opcode': changed['opcode'] = 126
                elif kind == 'component': changed['records'][0]['component'] = 3
                else: changed['records'][0]['code'] = '00' + changed['records'][0]['code'][2:]
                try: validate(changed)
                except ValueError as error: controls.append(dict(case=key, change=kind, reason=str(error)))
                else: raise ValueError('Corrupted gather evidence accepted')
        if report['dimension'] == 3 and report['opcode'] == 109 and report['format'] == 3:
            for kind in ('support', 'diagnostic', 'accepted'):
                changed = copy.deepcopy(report)
                if kind == 'support': changed['records'][0]['format_support'] |= 0x800000
                elif kind == 'diagnostic': changed['records'][0]['diagnostic_id'] = 0
                else: changed['records'][0]['supported'] = True
                try: validate(changed)
                except ValueError as error: controls.append(dict(case=key, change=kind, reason=str(error)))
                else: raise ValueError('Unsupported format evidence accepted')
    require(seen == expected, 'Complete device/stage/dimension/opcode/format matrix')
    log = args.test_log.read_text(encoding='utf-8-sig')
    require(len(re.findall(r'PASS\s+: DxbcGatherTests::originalShaders\(', log)) == len(expected) and
            re.search(r'Totals: \d+ passed, 0 failed, 0 skipped, 0 blacklisted,', log), 'Gather suite completion')
    root = Path(__file__).resolve().parents[1]
    paths = ['CMakeLists.txt', 'src/application/DxbcExecution.cpp', 'src/application/DxbcGpuOperations.cpp',
             'src/application/DxbcGpuOperations.h', 'src/application/DxbcTextureInputs.cpp',
             'src/application/DxbcTextureInputs.h', 'tests/DxbcGatherTests.cpp', 'tests/TextureOperationOracle.h',
             'tests/DxbcTextureLoadTests.cpp', 'tools/validate_native_gather.py', 'tools/validate_texture_precision.py']
    ledger = dict(schema=1, scope='native_texture_gather', passed=True, fixtures=len(seen),
                  direct_gpu_comparisons=count, unsupported_format_contracts=rejected, negative_controls=controls, full_debugger_parity=False,
                  renderdoc_removed=False, files=files + [identity(args.test_log)],
                  sources=[identity(root / p) for p in paths])
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(ledger, stream, indent=2); stream.write('\n')
    print(f'Validated {len(seen)} fixtures, {count} comparisons, {rejected} unsupported format contracts and {len(controls)} corruptions')


if __name__ == '__main__':
    main()
