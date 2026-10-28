"""Validate native class resource execution against retained original GPU outputs."""
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


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(report, original, bytecode):
    vendor, index, created, mode, second = (report[k] for k in ('vendor', 'index', 'created', 'mode', 'second'))
    require(report['decoy'] is False, 'Qualification matrix used the unrelated-binding control')
    require(vendor in (0, 4318, 32902) and index in (0, 1) and type(created) is bool and
            mode in range(4) and type(second) is bool, 'Invalid case identity')
    expected = (index if mode == 3 else (91 if index else 40) if mode else 30 + index) + 100 * second
    bits = struct.unpack('<I', struct.pack('<f', expected))[0]
    require(report['sample'] == (mode != 0) and report['expected'] == report['native'] == bits and
            report['original'] == original['original'], 'Native/authored value or original observation changed')
    gpu = report['gpu']
    require(gpu['vendor_id'] == (vendor or 5140) and gpu['warp'] == (vendor == 0) and
            gpu['backend'] == 'dxbc-gpu/25', 'Wrong GPU identity')
    require(0 < report['steps'] < 1000 and len(report['bindings']) == 1, 'Missing execution or binding')
    bound = report['bindings'][0]
    require(bound['observed_descriptor'] == original['descriptor'] and
            bound['instance_name'] == original['name'] and bound['type_name'] == original['type'],
            'Original class descriptor changed')
    texture = 5 + index if created else 2 * second + index
    sampler = 7 + index if created else 2 * second + index
    require(bound['this_bits'] == [0, 0, texture, sampler if mode or created else 0] and
            bound['this_valid'] == (15 if created else 12 if mode else 4), 'Wrong resolved class bases')
    require(bytecode[:4] == b'DXBC' and struct.unpack_from('<I', bytecode, 24)[0] == len(bytecode),
            'Invalid DXBC container')
    count = struct.unpack_from('<I', bytecode, 28)[0]
    parts = {}
    for offset in struct.unpack_from('<' + 'I' * count, bytecode, 32):
        tag, size = struct.unpack_from('<4sI', bytecode, offset)
        require(tag not in parts and offset + 8 + size <= len(bytecode), 'Invalid DXBC chunk')
        parts[tag] = bytecode[offset + 8:offset + 8 + size]
    words = struct.unpack('<' + 'I' * (len(parts[b'SHEX']) // 4), parts[b'SHEX'])
    require(words[0] == 0x50050 and words[1] == len(words), 'Wrong shader stage or length')
    pc, opcodes = 2, []
    while pc < len(words):
        opcode, size = words[pc] & 2047, words[pc] >> 24 & 127
        require(size and pc + size <= len(words), 'Invalid instruction length')
        opcodes.append(opcode)
        pc += size
    require(120 in opcodes and (45, 72, 109, 71)[mode] in opcodes and 168 in opcodes,
            'Fixture lost its class call, texture operation or output store')
    metadata = parts[b'IFCE']
    instances, types, records, slots, instance_offset, type_offset, slot_offset = struct.unpack_from('<7I', metadata)
    require(slots == 1 and records == 1 and instances == 2 and types == 2, 'Wrong class layout')
    if not created:
        matching = []
        for n in range(instances):
            offset = instance_offset + 16 * n
            name_offset, = struct.unpack_from('<I', metadata, offset)
            name = metadata[name_offset:].split(b'\0', 1)[0].decode('ascii')
            if name == original['name']:
                matching.append(struct.unpack_from('<6H', metadata, offset + 4))
        require(len(matching) == 1, 'Named class missing from original IFCE')
        type_id, size, cb, cb_offset, texture_base, sampler_base = matching[0]
        require(cb == cb_offset == 65535 and size == 2 and type_id == bound['shader_type'],
                'Unused constant buffer fields were not preserved as absent')
        require(texture_base + index == texture and (sampler_base + index == sampler if mode else sampler_base == 65535),
                'Resource class bases disagree with original IFCE')
    return report['original'] == bits


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--exe', type=Path, default=Path('build/vs2022/Release/FloraDxbcInterfaceResourceTests.exe'))
    parser.add_argument('--control', type=Path)
    parser.add_argument('--evidence', type=Path, action='append', default=[],
                        help='Additional retained files to pin, without expanding validation scope')
    args = parser.parse_args()
    log = args.log.read_text(encoding='utf-8')
    totals = re.search(r'Totals: (\d+) passed, (\d+) failed, (\d+) skipped', log)
    require(totals and 'Finished testing' in log, 'Incomplete Qt run')
    rows, mismatches = [], []
    for vendor, index, created, mode, second in itertools.product((0, 4318, 32902), range(2), range(2), range(4), range(2)):
        stem = args.directory / f'{vendor}-{index}-{created}-{mode}-{second}'
        report = json.loads(stem.with_suffix('.json').read_text())
        original = json.loads(stem.with_suffix('.original.json').read_text())
        bytecode = stem.with_suffix('.dxbc').read_bytes()
        require([report[k] for k in ('vendor', 'index', 'created', 'mode', 'second')] ==
                [vendor, index, bool(created), mode, bool(second)], 'Mismatched case file')
        if not validate(report, original, bytecode):
            mismatches.append(dict(case=stem.name, original=report['original'], native=report['native']))
        rows.append((report, original, bytecode))
    require(tuple(map(int, totals.groups())) == (98 - len(mismatches), len(mismatches), 0),
            'Qt failures do not match the retained original differences')
    rejected = 0
    for report, original, code in rows:
        for field in ('native', 'original', 'expected'):
            changed = copy.deepcopy(report)
            changed[field] ^= 1
            try:
                validate(changed, original, code)
            except ValueError:
                rejected += 1
            else:
                raise ValueError('Accepted a corrupted execution result')
    files = sorted(p for p in args.directory.iterdir() if p.is_file()) + [args.log]
    files += [Path(p) for p in ('tools/validate_native_interface_resources.py',
        'tests/DxbcInterfaceResourceTests.cpp', 'src/application/DxbcInterfaceBindings.cpp',
        'src/application/DxbcExecution.cpp', 'src/application/DxbcInputSnapshot.h', 'CMakeLists.txt')]
    files.append(args.exe)
    control_cases = 0
    if args.control:
        control = json.loads((args.control / 'control.json').read_text())
        require(control['evidence_complete'] is True and control['parity_passed'] is False and
                control['identical_executable_chunks'] is True, 'Incomplete binding control')
        observations = control['observations']
        expected_cases = {(f'{vendor}-0-1-2-{second}', decoy) for vendor in (0, 4318, 32902)
                          for second in range(2) for decoy in (False, True)}
        require(len(observations) == 12 and {(r['case'], r['decoy']) for r in observations} == expected_cases,
                'Incomplete unrelated-slot control matrix')
        from probe_interface_gather_binding import shader_code
        for row in observations:
            vendor, _, _, _, second = map(int, row['case'].split('-'))
            directory = args.control / ('decoy' if row['decoy'] else 'unbound')
            saved = json.loads((directory / (row['case'] + '.json')).read_text())
            native = struct.unpack('<I', struct.pack('<f', 40 + 100 * second))[0]
            original = struct.unpack('<I', struct.pack('<f',
                (240 if row['decoy'] else 0) + 100 * second if vendor == 32902 else 40 + 100 * second))[0]
            require(saved['decoy'] is row['decoy'] and row['native'] == row['expected'] == native and
                    row['original'] == saved['original'] == original and saved['native'] == native and
                    saved['bindings'][0]['this_bits'] == [0, 0, 5, 7], 'Binding control changed its observation')
            require(shader_code(args.control / 'unbound' / (row['case'] + '.dxbc')) ==
                    shader_code(args.control / 'decoy' / (row['case'] + '.dxbc')), 'Control shader instructions changed')
        for run in json.loads((args.control / 'runs.json').read_text()):
            require(not run['timed_out'] and run['timeout_seconds'] == 60 and run['exit_code'] != 0,
                    'Binding control timeout or lost failure status')
        for item in control['files']:
            require(digest(Path(item['path'])) == item['sha256'], 'Binding control artifact changed')
        files += sorted(p for p in args.control.rglob('*') if p.is_file())
        files.append(Path('tools/probe_interface_gather_binding.py'))
        control_cases = 12
    for path in args.evidence:
        require(path.is_file(), 'Additional evidence is not a saved file')
    files += args.evidence
    output = dict(schema=1, scope='native_interface_resource_operations', evidence_complete=True,
                  cases=96, rejected_corruptions=rejected, full_debug_parity=False,
                  parity_passed=not mismatches, original_mismatches=mismatches,
                  unrelated_binding_control_cases=control_cases,
                  files=[dict(path=p.as_posix(), bytes=p.stat().st_size, sha256=digest(p)) for p in files])
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(output, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: v for k, v in output.items() if k != 'files'}))
    if mismatches:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
