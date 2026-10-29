"""Check owned DXBC aggregation variants and independent non-tiled GPU results."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct


def require(value, message):
    if not value:
        raise ValueError(message)


def instructions(data):
    require(data[:4] == b'DXBC' and len(data) >= 32, 'Container header')
    size, count = struct.unpack_from('<II', data, 24)
    require(size == len(data) and 32 + count * 4 <= size, 'Container size')
    result = []
    for offset in struct.unpack_from(f'<{count}I', data, 32):
        require(offset + 8 <= size, 'Part header')
        tag, length = struct.unpack_from('<II', data, offset)
        require(offset + 8 + length <= size, 'Part length')
        if tag not in (0x58454853, 0x52444853):
            continue
        require(not result and length % 4 == 0, 'Shader parts')
        words = struct.unpack_from(f'<{length // 4}I', data, offset + 8)
        require(words[0] == 0x50050 and words[1] == len(words), 'Compute profile or size')
        position = 2
        while position < len(words):
            token = words[position]
            opcode = token & 2047
            length = words[position + 1] if opcode == 53 else (token >> 24) & 127
            require(length > 0 and position + length <= len(words), 'Instruction length')
            result.append(opcode)
            position += length
    require(result, 'Missing shader program')
    return result


def check(report):
    fields = report['fields']
    require(fields in (2, 3, 4) and report['vendor'] in (0, 4318, 32902), 'Matrix identity')
    expected = [sum(100 + 4 * fields + 4 * i + c for i in range(fields)) for c in range(3)] + [1]
    require(report['loads'] == fields and report['merges'] == fields - 1, 'Instruction counts')
    require(report['gpu'] == report['control'] == report['expected'] == expected, 'Independent GPU values')
    require(len(report['native']) == 2, 'Native mapping cases')
    for row, known in zip(report['native'], (False, True)):
        require(row['known_mapping'] is known and row['words'][:3] == expected[:3], 'Native data')
        require(row['validity'] == [1] * 12 + [int(known)] * 4, 'Native validity')
        require(row['steps'] > fields * 2, 'Execution progress')
        if known:
            require(row['words'] == expected, 'Known mapping status')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    paths = sorted(args.root.glob('*/report.json'))
    require(len(paths) == 9, 'Expected nine GPU cases')
    keys, hashes, rejected = set(), set(), 0
    for path in paths:
        report = json.loads(path.read_text(encoding='utf-8'))
        check(report)
        keys.add((report['vendor'], report['fields']))
        for name, digest, merge, checks in [('shader.dxbc', report['shader_sha256'], report['fields'] - 1, 1),
                                           ('original.dxbc', report['original_sha256'], 0, report['fields'])]:
            data = path.with_name(name).read_bytes()
            require(hashlib.sha256(data).hexdigest() == digest, 'Shader digest')
            opcodes = instructions(data)
            require(opcodes.count(227) == report['fields'] and opcodes.count(60) == merge and
                    opcodes.count(234) == checks, 'Independent opcode counts')
            hashes.add(digest)
        mutations = []
        for field in ('gpu', 'control', 'expected'):
            altered = copy.deepcopy(report)
            altered[field][0] ^= 1
            mutations.append(altered)
        for index in (0, 1):
            altered = copy.deepcopy(report)
            altered['native'][index]['validity'][12] ^= 1
            mutations.append(altered)
        for altered in mutations:
            try:
                check(altered)
            except ValueError:
                rejected += 1
            else:
                raise ValueError('Corrupted report accepted')
    require(keys == {(v, f) for v in (0, 4318, 32902) for f in (2, 3, 4)}, 'Incomplete matrix')
    require(len(hashes) == 6, 'Unexpected compiler or aggregate variant diversity')
    result = dict(schema=1, status='verified', gpu_cases=len(keys), gpu_dispatches=2 * len(keys),
                  native_executions=2 * len(keys), shader_variants=len(hashes), corruptions_rejected=rejected,
                  tiled_mapping_verified=False, full_debugger_parity=False)
    args.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
