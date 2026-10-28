"""Validate planar SRV execution, device rejection contracts and GPA round trips."""
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
    require(report['schema'] == 1 and report['probe'] == 'native_planar_srv' and report['passed'] is True,
            'Planar evidence identity')
    vendor, stage, fmt, plane, integer, array = (report[k] for k in
        ('vendor', 'stage', 'format', 'plane', 'integer', 'array'))
    require(vendor in (0, 4318, 32902) and stage in (0, 1, 5) and fmt in (103, 104, 105)
            and plane in (0, 1) and type(integer) is bool and type(array) is bool, 'Planar configuration')
    typed = ((49 if plane else 61) if fmt == 103 else (35 if plane else 56)) + int(integer)
    require(report['view_format'] == typed, 'Typed plane interpretation')
    helper = report['helper']
    require(helper['backend'] == 'dxbc-gpu/25' and helper['warp'] is (vendor == 0)
            and helper['vendor_id'] == (5140 if vendor == 0 else vendor)
            and helper['feature_level'] >= 0xb000 and helper['device_id'] > 0
            and helper['luid_low'] | helper['luid_high'], 'Exact selected adapter and backend')
    capability = report['capability']
    require(type(capability['supported']) is bool, 'Explicit capability outcome')
    if not capability['supported']:
        require(capability['create_status'] == 0x80070057 and capability['support'] & 0x20 == 0
                and any(d['id'] == 92 and d['severity'] == 1 for d in capability['diagnostics']),
                'Independent unsupported-format diagnosis')
        require(report['native_rejection'] == 'Create private texture snapshot: HRESULT 0x80070057'
                and report['records'] == [], 'Native rejection without fabricated comparisons')
        return
    require(capability['format_status'] == 0 and capability['create_status'] == 0
            and capability['support'] & 0x20 and not report['native_rejection']
            and all(d['severity'] > 1 for d in capability['diagnostics']), 'Supported texture creation')
    operations = (45, 61) if integer else (45, 61, 72, 109)
    require([(r['operation'], r['mode']) for r in report['records']] ==
            list(itertools.product(operations, range(4))), 'Complete operation/mode matrix')
    for row in report['records']:
        opcode, mode = row['operation'], row['mode']
        mask = 9 if mode & 1 else 15
        relative = int(array and mode != 3)
        address = ([4 if plane else 8, 1, relative, 0] if mode == 2 else [1, 1, relative, 0]) if opcode == 45 else \
                  [int(mode == 2), 0, 0, 0] if opcode == 61 else \
                  list(struct.unpack('<4I', struct.pack('<4f', .4375, .5625, float(relative), 0)))
        require(row['address'] == address and row['first_layer'] == int(array and mode == 3)
                and row['plane_explicit'] is (mode != 1), 'Plane, slice and operand selection')
        require(len(row['actual']) == 4 and row['actual'] == row['expected'] and row['valid'] == 15
                and row['other_plane_valid'] == 15, 'Exact result and unrelated-plane validity')
        unknown = 15 if opcode == 61 or (opcode == 45 and mode == 2) else 7 if opcode == 45 else 15 ^ mask
        require(row['unknown_valid'] == unknown, 'Selected-plane validity and spatial bounds')
        require(type(row['assisted']) is bool and (row['assisted'] or opcode in (45, 61)), 'GPU operation provenance')
        for component, seed in enumerate((17, 23, 31, 47)):
            if not mask & 1 << component:
                require(row['actual'][component] == seed, 'Untouched destination bits')
        header, code = instructions(row['code'])
        require(header == stage << 16 | 0x50, 'Original shader stage')
        dimension, returns = (8 if array else 3), (4 if integer else 5)
        require(any(r[0] & 2047 == 88 and r[0] >> 11 & 31 == dimension
                    and list(r[1:]) == [0x1070f2, 3, returns * 0x1111] for r in code), 'Original typed resource')
        require(any(r[0] & 2047 == 89 and list(r[1:]) == [0x208e46, 2, 2] for r in code), 'Original constants')
        selected = [r for r in code if r[0] & 2047 in (45, 61, 72, 109)]
        require(len(selected) == 1, 'One original texture operation')
        instruction, = selected
        query = 2 if mode == 0 else 0 if mode == 1 else 1
        require(instruction[0] & 0xffffff == opcode | (query << 11 if opcode == 61 else 0), 'Opcode controls')
        operands = [0x100002 | mask << 4, 3, 0x20800a if opcode == 61 else 0x208e46, 2, 0,
                    0x107e46 if opcode == 61 else 0x1071b6, 3]
        if opcode == 72: operands += [0x106000, 4, 0x20800a, 2, 1]
        if opcode == 109: operands += [0x10600a | (mode % 2) << 4, 4]
        require(list(instruction[1:]) == operands, 'Original operands, swizzle and mask')
        if opcode in (72, 109):
            require(any(r[0] & 2047 == 90 and list(r[1:]) == [0x106000, 4] for r in code), 'Original sampler')
        require(any(r[0] & 2047 == 54 and list(r[1:]) == [0x1000f2, 3, 0x4002, 17, 23, 31, 47]
                    for r in code), 'Original destination initialization')
        output = [0x11e0f2, 3, 0x4001, 0, 0x4001, 0, 0x100e46, 3] if stage == 5 else [0x1020f2, 0, 0x100e46, 3]
        require(any(r[0] & 2047 == (168 if stage == 5 else 54) and list(r[1:]) == output for r in code),
                'Independent raw output transport')
        frame = row['frame']
        if stage == 5 and fmt == 103 and opcode == 45 and mode == 0:
            require(isinstance(frame, dict) and re.fullmatch('[0-9a-f]{64}', frame['frame_sha256'])
                    and frame['event'] == 100 and frame['shader'] == 15 and frame['resource'] == 4
                    and frame['view'] == 6 and frame['plane'] == plane and frame['result'] == row['actual']
                    and frame['source_unchanged'] is True and frame['helper'] == helper, 'GPA snapshot and replay provenance')
        else:
            require(frame is None, 'No invented frame qualification')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--test-log', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    def identity(path):
        return dict(path=path.resolve().as_posix(), bytes=path.stat().st_size,
                    sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    expected = set(itertools.product((0, 4318, 32902), (0, 1, 5), (103, 104, 105), (0, 1), (False, True), (False, True)))
    seen, files, controls = set(), [], []
    comparisons = rejected = frames = 0
    for path in sorted(args.root.glob('*.json')):
        report = json.loads(path.read_text())
        validate(report)
        key = tuple(report[k] for k in ('vendor', 'stage', 'format', 'plane', 'integer', 'array'))
        require(key not in seen, 'Unique planar fixture')
        seen.add(key); files.append(identity(path)); comparisons += len(report['records'])
        rejected += not report['capability']['supported']
        for row in report['records']:
            if row['frame'] is not None:
                frame_path = path.with_suffix('.gpa_frame')
                evidence = identity(frame_path)
                require(evidence['sha256'] == row['frame']['frame_sha256'], 'Preserved GPA frame identity')
                files.append(evidence); frames += 1
        if report['stage'] == 5:
            kinds = ('bits', 'validity', 'unknown', 'other_plane', 'address', 'plane', 'missing', 'code', 'backend') \
                    if report['capability']['supported'] else ('support', 'diagnosis', 'rejection', 'backend')
            if report['format'] == 103: kinds += ('frame',)
            for kind in kinds:
                changed = copy.deepcopy(report)
                row = changed['records'][0] if changed['records'] else None
                if kind == 'bits': row['actual'][0] ^= 1
                elif kind == 'validity': row['valid'] = 0
                elif kind == 'unknown': row['unknown_valid'] ^= 1
                elif kind == 'other_plane': row['other_plane_valid'] = 0
                elif kind == 'address': row['address'][0] += 1
                elif kind == 'plane': row['plane_explicit'] = False
                elif kind == 'missing': changed['records'].pop()
                elif kind == 'code': row['code'] = '00' + row['code'][2:]
                elif kind == 'frame': row['frame']['source_unchanged'] = False
                elif kind == 'support': changed['capability']['support'] |= 0x20
                elif kind == 'diagnosis': changed['capability']['diagnostics'] = []
                elif kind == 'rejection': changed['native_rejection'] = ''
                else: changed['helper']['backend'] = 'dxbc-gpu/24'
                try: validate(changed)
                except ValueError as error: controls.append(dict(case=key, change=kind, reason=str(error)))
                else: raise ValueError('Corrupted planar evidence accepted')
    require(seen == expected, 'Complete planar device/stage/format/plane/type/array matrix')
    log = args.test_log.read_text(encoding='utf-8-sig')
    require(len(re.findall(r'PASS\s+: DxbcPlanarTests::originalShaders\(', log)) == len(expected)
            and 'PASS   : DxbcPlanarTests::invalidMetadata()' in log
            and re.search(r'Totals: 219 passed, 0 failed, 0 skipped, 0 blacklisted,', log), 'Completed planar suite')
    repo = Path(__file__).resolve().parents[1]
    sources = ['src/application/DxbcGpuOperations.cpp', 'src/application/DxbcResourceInfo.cpp',
               'src/application/DxbcTextureInputs.cpp', 'src/application/DxbcTextureInputs.h',
               'tests/DxbcPlanarTests.cpp', 'tests/TextureOperationOracle.h', 'tools/validate_native_planar.py',
               'tools/validate_texture_precision.py']
    ledger = dict(schema=1, scope='native_planar_srv', passed=True, fixtures=len(seen),
                  direct_gpu_comparisons=comparisons, unsupported_source_contracts=rejected,
                  frame_roundtrips=frames, negative_controls=controls, full_debugger_parity=False,
                  renderdoc_removed=False, files=files + [identity(args.test_log)],
                  sources=[identity(repo / p) for p in sources])
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(ledger, stream, indent=2); stream.write('\n')
    print(f'Validated {len(seen)} fixtures, {comparisons} comparisons, {frames} frames, '
          f'{rejected} rejection contracts and {len(controls)} corruptions')


if __name__ == '__main__':
    main()
