"""Validate native minimum-precision gather against direct original DXBC execution."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path
import re
import struct
from validate_texture_precision import instructions, require


def annotated(words, precision, modifier=0):
    return [words[0] | 0x80000000, 1 | precision << 14 | modifier << 6] + words[1:] if precision or modifier else words


def validate(report):
    require(report['schema'] == 1 and report['probe'] == 'minimum_precision_gather' and report['passed'] is True,
            'Gather precision identity')
    vendor, stage, dimension, opcode, precision = (report[k] for k in ('vendor', 'stage', 'dimension', 'opcode', 'precision'))
    comparison, programmable = opcode in (126, 128), opcode in (127, 128)
    require(vendor in (0, 4318, 32902) and stage in (0, 1, 5) and dimension in (3, 6, 8, 10)
            and opcode in (109, 126, 127, 128) and precision in (1, 2)
            and (not programmable or dimension in (3, 8)), 'Supported gather precision configuration')
    helper = report['helper']
    require(helper['backend'] == 'dxbc-gpu/24' and helper['warp'] == (vendor == 0)
            and helper['vendor_id'] == (5140 if vendor == 0 else vendor), 'Selected GPU identity')
    expected_rows = set(itertools.product(range(8), range(1 if stage == 5 else 2)))
    rows = report['records']
    require(len(rows) == len(expected_rows) and {(r['mode'], r['direct']) for r in rows} == expected_rows,
            'Complete source/output selection matrix')
    for row in rows:
        mode, direct = row['mode'], row['direct']
        mask = 9 if mode & 1 else 15
        precisions = [0 if mode == 2 else precision, precision if mode else 0,
                      (5 if mode & 1 else 4) if programmable and mode else 0,
                      precision if comparison and mode else 0]
        modifiers = [mode % 4, 0, mode % 4 if comparison else 0]
        literal = [False, False, False]
        if mode >= 4: literal[1 if mode == 5 and programmable else 2 if mode == 6 and comparison else 0] = True
        component = 0 if comparison else mode % 4
        require(row['precision'] == precisions and row['modifiers'] == modifiers and row['literal'] == literal
                and row['component'] == component and row['mask'] == mask, 'Original precision and selection contract')
        coords = [1, .2501, -.5001, 1] if dimension in (6, 10) else [.43749, .56251, 1, 0]
        address = list(struct.unpack('<4I', struct.pack('<4f', *coords)))
        values = [address, [31, 17, 0, 0], [0x3f000901, 0, 0, 0]]
        require([row['address'], row['offset'], row['reference']] == values, 'Raw source inputs')
        require(row['expected'] == row['actual'] and len(row['actual']) == 4 and row['valid'] == 15
                and all(type(v) is int and 0 <= v <= 0xffffffff for v in row['actual']), 'Direct original GPU result')
        require(row['unknown_valid'] == 15 ^ mask, 'Unknown texture validity')
        source_validity = [(-1 if (index == 1 and not programmable) or (index == 2 and not comparison)
                            else 15 if literal[index] else 15 ^ mask) for index in range(3)]
        require(row['source_validity'] == source_validity, 'Unknown constants versus literal provenance')
        require(row['trace_roundtrip'] is (mode == 0 and not direct and dimension == 3), 'Selected precision journal round trip')
        for index, seed in enumerate((0x3e000000, 0x3e800000, 0x3f000000, 0x3f400000)):
            if not mask & (1 << index): require(row['actual'][index] == seed, 'Preserved destination component')
        header, code = instructions(row['code'])
        require(header == stage << 16 | 0x50, 'Original shader stage')
        selected = [r for r in code if r[0] & 2047 in (109, 126, 127, 128)]
        require(len(selected) == 1, 'One original gather instruction')
        instruction, = selected
        require(instruction[0] & 2047 == opcode and list(instruction[1:4]) ==
                [0x80000001, 0x80000002 | dimension << 6, 3 | 0x5555 << 6], 'Typed original gather framing')
        expected = annotated([0x100002 | (2 if direct else 0) << 12 | mask << 4, 0 if direct else 3], precisions[0])
        def operand(index):
            value = values[index]
            source = ([0x4001, value[0]] if index == 2 else [0x4002] + value) if literal[index] else \
                     [0x20800a if index == 2 else 0x208e46, 2, index]
            return annotated(source, precisions[index + 1], modifiers[index])
        expected += operand(0)
        if programmable: expected += operand(1)
        expected += [0x1071b6, 3, 0x10600a | component << 4, 4]
        if comparison: expected += operand(2)
        require(list(instruction[4:]) == expected, 'Original operand precision, modifiers, literals and bindings')
        require(any(r[0] & 0x00ffffff == 106 | 1 << 16 | ((mode & 1) << 11) for r in code), 'Global precision/refactoring flags')
        require(any(r[0] & 2047 == 88 and r[0] >> 11 & 31 == dimension and list(r[1:]) == [0x1070f2, 3, 0x5555]
                    for r in code), 'Original resource binding')
        require(any(r[0] & 2047 == 90 and r[0] >> 11 & 15 == int(comparison) and list(r[1:]) == [0x106000, 4]
                    for r in code), 'Original sampler binding')
        if stage != 5:
            require(any(r[0] & 2047 == 101 and list(r[1:]) == annotated([0x1020f2, 0], precisions[0] if direct else 0)
                        for r in code), 'Original output precision declaration')
        if not direct:
            output = 0x100002 | (0 if stage == 5 else 2) << 12
            index = 2 if stage == 5 else 0
            require(any(r[0] & 2047 == 54 and list(r[1:]) == [output | 15 << 4, index, 0x100e46, 3]
                        for r in code) and any(r[0] & 2047 == 54 and list(r[1:]) ==
                        [output | mask << 4, index] + annotated([0x100e46, 3], precisions[0]) for r in code),
                    'Independent untouched-component transport and selected precision promotion')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--test-log', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    def identity(path):
        return dict(path=path.resolve().as_posix(), bytes=path.stat().st_size,
                    sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    expected = {(v, s, d, o, p) for v, s, d, o, p in itertools.product((0, 4318, 32902), (0, 1, 5),
                 (3, 6, 8, 10), (109, 126, 127, 128), (1, 2)) if o not in (127, 128) or d in (3, 8)}
    seen, files, controls, comparisons = set(), [], [], 0
    for path in sorted(args.root.glob('*.json')):
        report = json.loads(path.read_text())
        validate(report)
        key = tuple(report[k] for k in ('vendor', 'stage', 'dimension', 'opcode', 'precision'))
        require(key not in seen, 'Unique precision fixture')
        seen.add(key); files.append(identity(path)); comparisons += len(report['records'])
        if report['dimension'] == 3 and report['opcode'] == 128:
            for kind in ('bits', 'validity', 'unknown', 'source', 'trace', 'precision', 'modifier', 'literal', 'missing', 'code', 'backend'):
                changed = copy.deepcopy(report)
                row = changed['records'][0]
                if kind == 'bits': row['actual'][0] ^= 1
                elif kind == 'validity': row['valid'] = 0
                elif kind == 'unknown': row['unknown_valid'] ^= 1
                elif kind == 'source': row['source_validity'][0] = 15
                elif kind == 'trace': row['trace_roundtrip'] = False
                elif kind == 'precision': row['precision'][0] = 0
                elif kind == 'modifier': row['modifiers'][0] = 1
                elif kind == 'literal': row['literal'][0] = True
                elif kind == 'missing': changed['records'].pop()
                elif kind == 'backend': changed['helper']['backend'] = 'dxbc-gpu/23'
                else: row['code'] = '00' + row['code'][2:]
                try: validate(changed)
                except ValueError as error: controls.append(dict(case=key, change=kind, reason=str(error)))
                else: raise ValueError('Corrupted gather precision evidence accepted')
    require(seen == expected, 'Complete device/stage/dimension/opcode/precision matrix')
    log = args.test_log.read_text(encoding='utf-8-sig')
    require(len(re.findall(r'PASS\s+: DxbcGatherTests::minimumPrecision\(', log)) == len(expected) and
            re.search(r'Totals: \d+ passed, 0 failed, 0 skipped, 0 blacklisted,', log), 'Completed precision suite')
    repo = Path(__file__).resolve().parents[1]
    sources = ['src/application/DxbcExecution.cpp', 'src/application/DxbcGpuOperations.cpp',
               'src/application/DxbcTextureInputs.cpp', 'src/application/DxbcTextureInputs.h',
               'tests/DxbcGatherTests.cpp', 'tests/TextureOperationOracle.h',
               'tools/validate_gather_precision.py', 'tools/validate_texture_precision.py']
    ledger = dict(schema=1, scope='native_minimum_precision_gather', passed=True, fixtures=len(seen),
                  direct_gpu_comparisons=comparisons, negative_controls=controls,
                  full_debugger_parity=False, renderdoc_removed=False,
                  files=files + [identity(args.test_log)], sources=[identity(repo / p) for p in sources])
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(ledger, stream, indent=2); stream.write('\n')
    print(f'Validated {len(seen)} fixtures, {comparisons} direct comparisons and {len(controls)} corruptions')


if __name__ == '__main__':
    main()
