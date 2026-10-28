"""Validate class buffer/query evidence without treating original failures as passes."""
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


def opcodes(data):
    require(data[:4] == b'DXBC' and struct.unpack_from('<I', data, 24)[0] == len(data),
            'Invalid shader container')
    count, = struct.unpack_from('<I', data, 28)
    parts = {}
    for offset in struct.unpack_from('<' + 'I' * count, data, 32):
        tag, size = struct.unpack_from('<4sI', data, offset)
        require(tag not in parts and offset + 8 + size <= len(data), 'Invalid shader chunk')
        parts[tag] = data[offset + 8:offset + 8 + size]
    words = struct.unpack('<' + 'I' * (len(parts[b'SHEX']) // 4), parts[b'SHEX'])
    require(words[0] == 0x50050 and words[1] == len(words), 'Wrong shader stage or length')
    pc, result = 2, []
    while pc < len(words):
        opcode, size = words[pc] & 2047, words[pc] >> 24 & 127
        require(size and pc + size <= len(words), 'Invalid instruction framing')
        result.append(opcode)
        pc += size
    return result


def validate(case, native, control, code, direct):
    vendor, index, created, mode, second = case
    expected = (122 + 8 * index, 134 + 5 * index, 107 + 2 * index, 3 + (6 << index))[mode]
    expected += 100 * second
    require(native['native'] == native['expected'] == control['control'] == control['expected'] == expected,
            'Native or direct GPU result disagrees with authored resource contents')
    require(native['valid'] == [1, 1, 1, 1], 'Native output contains unknown bytes')
    require(0 < native['steps'] < 1000, 'Missing bounded native execution')
    gpu = native['gpu']
    require(gpu['vendor_id'] == (vendor or 5140) and gpu['warp'] == (vendor == 0),
            'Wrong adapter selection')
    require(len(native['bindings']) == 1, 'Missing class binding')
    binding = native['bindings'][0]
    slot = 5 + index if created else 2 * second + index
    require(binding['this_bits'] == [0, 0, slot, 0] and
            binding['this_valid'] == (15 if created else 4), 'Wrong resolved class resource base')
    class_ops, direct_ops = opcodes(code), opcodes(direct)
    query = 61 if mode == 3 else 121
    require(120 in class_ops and 120 not in direct_ops and query in class_ops and query in direct_ops,
            'Missing class dispatch or query; direct control must not dispatch a class')
    if mode != 3:
        require((165, 167, 45)[mode] in class_ops and (165, 167, 45)[mode] in direct_ops,
                'Missing resource load')
    require(168 in class_ops and 168 in direct_ops, 'Missing original result store')
    return expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--log', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--exe', type=Path,
                        default=Path('build/vs2022/Release/FloraDxbcInterfaceBufferTests.exe'))
    parser.add_argument('--evidence', action='append', type=Path, default=[])
    args = parser.parse_args()
    rows, rejected, failures = [], 0, 0
    for case in itertools.product((0, 4318, 32902), range(2), range(2), range(4), range(2)):
        stem = args.directory / '-'.join(map(str, case))
        read = lambda suffix: json.loads(Path(str(stem) + suffix).read_text())
        native, control, status = (read(s) for s in ('.native.json', '.control.json', '.status.json'))
        code = stem.with_suffix('.dxbc').read_bytes()
        direct = Path(str(stem) + '.control.dxbc').read_bytes()
        expected = validate(case, native, control, code, direct)
        for target in ('native', 'control'):
            modified_native, modified_control = copy.deepcopy(native), copy.deepcopy(control)
            (modified_native if target == 'native' else modified_control)[target] ^= 1
            try:
                validate(case, modified_native, modified_control, code, direct)
            except ValueError:
                rejected += 1
            else:
                raise ValueError('Accepted a corrupted observed result')
        require(status['completed'] is True, 'A child exceeded its deadline')
        child = Path(str(stem) + '.child.txt').read_text(encoding='utf-8')
        require('Dispatched original shader (class: 0)' in child and
                'Dispatching original shader (class: 1)' in child, 'Missing direct/class execution boundary')
        row = dict(case=stem.name, native=expected, control=expected, status=status)
        if status['crashed']:
            require(status['exit_code'] != 0 and 'Dispatched original shader (class: 1)' not in child and
                    not stem.with_suffix('.json').exists() and
                    not Path(str(stem) + '.original.json').exists(), 'Inconsistent original crash evidence')
            row['outcome'] = 'original_dispatch_crash'
            failures += 1
        else:
            report, original = read('.json'), read('.original.json')
            require([report[k] for k in ('vendor', 'index', 'created', 'mode', 'second')] == list(case),
                    'Case identity changed')
            require(report['native'] == report['expected'] == expected and
                    report['original'] == original['original'] and
                    report['bindings'] == native['bindings'] and
                    original['descriptor'] == native['bindings'][0]['observed_descriptor'],
                    'Original report changed')
            mismatch = original['original'] != expected
            require(status['exit_code'] == int(mismatch) and 'Finished testing' in child,
                    'Child failure was not an original value difference')
            if mismatch:
                require('Actual   (original)' in child, 'Missing original comparison failure')
                failures += 1
            row.update(original=original['original'], outcome='original_value_difference' if mismatch else 'match')
        rows.append(row)
    log = args.log.read_text(encoding='utf-8')
    totals = re.findall(r'Totals: (\d+) passed, (\d+) failed, (\d+) skipped', log)
    require(totals and 'Finished testing' in log and
            tuple(map(int, totals[-1])) == (98 - failures, failures, 0), 'Parent test status mismatch')
    paths = sorted(p for p in args.directory.iterdir() if p.is_file())
    paths += [args.log, args.exe, Path(__file__), Path('tests/DxbcInterfaceBufferTests.cpp'),
              Path('tests/DxbcMemoryTests.cpp'), Path('src/application/DxbcExecution.cpp'),
              Path('src/application/DxbcInputSnapshot.h'), Path('CMakeLists.txt')] + args.evidence
    files = [dict(path=p.resolve().relative_to(Path.cwd()).as_posix(), bytes=p.stat().st_size,
                  sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in paths]
    output = dict(schema=1, scope='native_class_buffer_queries', evidence_complete=True,
                  parity_passed=failures == 0, full_debug_parity=False, native_cases=96,
                  direct_gpu_controls=96, original_matches=96 - failures, rejected_corruptions=rejected,
                  cases=rows, files=files)
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(output, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: v for k, v in output.items() if k not in ('cases', 'files')}))
    return int(failures != 0)


if __name__ == '__main__':
    raise SystemExit(main())
