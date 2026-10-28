"""Independently check captured GPA record memory, wire bytes and research provenance."""
import argparse
import copy
import json
from pathlib import Path
import struct

from probe_original_debugger_wire import PLAYER_SHA256, SHA256, digest, require
from probe_original_debugger_records import fixture


def decode_wire(data):
    """Parse observed bytes without calling the original codec or probe encoder."""
    cursor = 0
    count = 0

    def take(size):
        nonlocal cursor
        require(0 <= size <= 65536 and cursor + size <= len(data), 'Truncated packet')
        value = data[cursor:cursor + size]
        cursor += size
        return value

    def integer(size):
        return int.from_bytes(take(size), 'little')

    def string():
        size = integer(8)
        require(size <= 15, 'Unexpected heap string')
        return take(size).decode('ascii')

    def vector(reader):
        size = integer(8)
        require(size <= 32, 'Record count bound exceeded')
        return [reader() for _ in range(size)]

    def record():
        nonlocal count
        count += 1
        require(count <= 32, 'Recursive record bound exceeded')
        return dict(u32_00=integer(4), u32_04=integer(4), string_08=string(), u32_28=integer(4),
                    bytes_30=take(128).hex(), byte_b0=integer(1), records_b8=vector(record))

    def group():
        return dict(records_00=vector(record), records_18=vector(record), records_30=vector(record),
                    u32_48=integer(4), u32_4c=integer(4))

    result = dict(records_20=vector(record), records_38=vector(record), records_50=vector(group),
                  string_68=string(), strings_88=vector(string), string_a0=string())
    require(cursor == len(data), 'Trailing packet data')
    return result


def check_memory(blocks, model, populated):
    """Rebuild every guarded byte from the fixture and recorded allocation addresses."""
    by_label = {row['label']: row for row in blocks}
    require(len(by_label) == len(blocks), 'Duplicate allocation label')
    used = set()
    ranges = []
    for row in blocks:
        require(type(row['address']) is int and row['address'] > 32 and
                type(row['bytes']) is int and 0 < row['bytes'] <= 65536, 'Invalid allocation')
        ranges.append((row['address'] - 32, row['address'] + row['bytes'] + 32))
    ranges.sort()
    require(all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:])), 'Overlapping allocations')

    def string(buffer, offset, value):
        value = value.encode('ascii') if populated else b''
        buffer[offset:offset + 32] = value + bytes(16 - len(value)) + struct.pack('<QQ', len(value), 15)

    def vector(buffer, offset, label, rows, stride, write):
        start = by_label[label]['address'] if rows else 0
        buffer[offset:offset + 24] = struct.pack('<QQQ', start, start + len(rows) * stride,
                                                start + len(rows) * stride)
        if rows:
            block = bytearray([0xa5] * (len(rows) * stride))
            for index, row in enumerate(rows):
                write(block, index * stride, label + '/' + str(index), row)
            verify(label, block)

    def records(buffer, offset, label, rows):
        vector(buffer, offset, label, rows, 0xd0, record)

    def record(buffer, offset, label, row):
        if populated:
            struct.pack_into('<II', buffer, offset, row['u32_00'], row['u32_04'])
            struct.pack_into('<I', buffer, offset + 0x28, row['u32_28'])
            buffer[offset + 0x30:offset + 0xb0] = bytes.fromhex(row['bytes_30'])
            buffer[offset + 0xb0] = row['byte_b0']
        string(buffer, offset + 8, row['string_08'])
        records(buffer, offset + 0xb8, label + '/records_b8', row['records_b8'])

    def group(buffer, offset, label, row):
        for displacement, key in ((0, 'records_00'), (0x18, 'records_18'), (0x30, 'records_30')):
            records(buffer, offset + displacement, label + '/' + key, row[key])
        if populated:
            struct.pack_into('<II', buffer, offset + 0x48, row['u32_48'], row['u32_4c'])

    def verify(label, expected):
        row = by_label[label]
        require(row['bytes'] == len(expected) and bytes.fromhex(row['guarded_hex']) ==
                bytes([0xa5] * 32) + expected + bytes([0xa5] * 32), 'Memory mismatch: ' + label)
        used.add(label)

    root = bytearray([0xa5] * 0xc0)
    records(root, 0x20, 'result/records_20', model['records_20'])
    records(root, 0x38, 'result/records_38', model['records_38'])
    vector(root, 0x50, 'result/records_50', model['records_50'], 0x50, group)
    string(root, 0x68, model['string_68'])
    vector(root, 0x88, 'result/strings_88', model['strings_88'], 0x20,
           lambda block, offset, label, value: string(block, offset, value))
    string(root, 0xa0, model['string_a0'])
    verify('result', root)
    require(used == set(by_label), 'Unexpected allocation')


def validate_case(row):
    model = fixture(row['kind'], row['seed'])
    require(row['model'] == model, 'Fixture changed')
    require(decode_wire(bytes.fromhex(row['wire_hex'])) == model, 'Decoded wire differs from fixture')
    for key, populated in (('source_before', True), ('source_after', True), ('target_before', False),
                            ('target_after', True), ('target_after_empty_reuse', True)):
        check_memory(row[key], model, populated)
    require(row['source_before'] == row['source_after'], 'Source mutated')
    require(row['target_after'] == row['target_after_empty_reuse'], 'Empty reuse did not preserve old memory')
    require([(b['label'], b['address'], b['bytes']) for b in row['target_before']] ==
            [(b['label'], b['address'], b['bytes']) for b in row['target_after']], 'Target allocations changed')
    # Packet fields fix transfer boundaries independently of the observed chunk list.
    sizes = []

    def string(value):
        sizes.extend((8, len(value.encode('ascii'))))

    def records(rows):
        sizes.append(8)
        for value in rows:
            sizes.extend((4, 4))
            string(value['string_08'])
            sizes.extend((4, 128, 1))
            records(value['records_b8'])

    records(model['records_20'])
    records(model['records_38'])
    sizes.append(8)
    for value in model['records_50']:
        for key in ('records_00', 'records_18', 'records_30'):
            records(value[key])
        sizes.extend((4, 4))
    string(model['string_68'])
    sizes.append(8)
    for value in model['strings_88']:
        string(value)
    string(model['string_a0'])
    require(row['write_sizes'] == sizes and row['read_sizes'] == [size for size in sizes if size] and
            row['empty_reuse_read_sizes'] == [8] * 6, 'Transfer boundaries changed')


def corruption_controls(rows):
    count = 0
    for row in rows:
        mutations = []
        for key in ('source_after', 'target_before', 'target_after', 'target_after_empty_reuse'):
            # Root guard, untransmitted request bytes, vector pointer, and final guard.
            for offset in (0, 32, 32 + 0x20, -1):
                changed = copy.deepcopy(row)
                raw = bytearray.fromhex(changed[key][0]['guarded_hex'])
                raw[offset] ^= 1
                changed[key][0]['guarded_hex'] = raw.hex()
                mutations.append(changed)
        for key in ('wire_hex',):
            changed = copy.deepcopy(row)
            raw = bytearray.fromhex(changed[key])
            raw[-1] ^= 1
            changed[key] = raw.hex()
            mutations.append(changed)
        changed = copy.deepcopy(row)
        changed['read_sizes'][0] = 4
        mutations.append(changed)
        # A raw record byte must be checked independently of the wire packet.
        changed = copy.deepcopy(row)
        block = next(b for b in changed['target_after'] if b['label'].endswith(('records_20', 'records_38', 'records_00')))
        raw = bytearray.fromhex(block['guarded_hex'])
        raw[32 + 0x30] ^= 1
        block['guarded_hex'] = raw.hex()
        mutations.append(changed)
        for changed in mutations:
            try:
                validate_case(changed)
            except (ValueError, KeyError, UnicodeError):
                count += 1
            else:
                raise ValueError('Corrupted record accepted')
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--runtime', default='final', help='Runtime subdirectory within the evidence root')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    root = args.directory
    static, runtime = root / 'static-final', root / args.runtime
    report = json.loads((runtime / 'records.json').read_text(encoding='utf-8'))
    require(report['schema'] == 1 and report['scope'] == 'original_debugger_nonempty_records' and
            report['completed'] is True and report['engine_recovered'] is False and
            report['shader_debug_parity'] is False, 'Wrong report scope or completion')
    require(report['source_sha256'] == digest(Path('tools/probe_original_debugger_records.py')), 'Probe changed')
    process = json.loads((runtime / 'process.json').read_text(encoding='utf-8'))
    require(process['exit_code'] == 0 and process['timed_out'] is False and process['timeout_seconds'] == 30,
            'Probe did not complete within deadline')
    hashes = {'dx11_player.dll': PLAYER_SHA256, 'dx11_playback.dll': SHA256}
    require({Path(b['path']).name: b['sha256'] for b in report['binaries']} == hashes, 'Wrong binary matrix')
    for row in report['binaries']:
        require(digest(Path(row['path'])) == row['sha256'], 'Original binary changed')
    exports = {}
    for name, sha in hashes.items():
        text = (static / (name + '.txt')).read_text(encoding='utf-8')
        require(text.splitlines()[0] == f'PROGRAM {name} SHA256 {sha} BASE 180000000' and
                text.splitlines()[-1] == 'COMPLETE 7' and 'NO FUNCTION ' not in text, 'Incomplete Ghidra export')
        require('ERROR REPORT SCRIPT ERROR' not in (static / (name + '.log')).read_text(encoding='utf-8-sig'),
                'Failed Ghidra script')
        exports[name] = text
    for line in ('ASM RVA 0x10d1c3 MOV R8D,0x80', 'ASM RVA 0x10d20c CALL 0x18010d0b0',
                 'ASM RVA 0x10982c CALL 0x18010dbd0'):
        require(line in exports['dx11_player.dll'], 'Missing writer call evidence')
    for line in ('ASM RVA 0x261611 MOV R8D,0x80', 'ASM RVA 0x261658 CALL 0x180261470',
                 'ASM RVA 0x261352 CALL 0x180261470'):
        require(line in exports['dx11_playback.dll'], 'Missing reader call evidence')
    for row in json.loads((static / 'evidence-sha256.json').read_text(encoding='utf-8-sig')):
        require(digest(static / row['name']) == row['sha256'], 'Ghidra evidence changed')
    cases = report['cases']
    require(len(cases) == 12 and {(r['kind'], r['seed']) for r in cases} ==
            {(k, s) for k in ('leaf', 'nested', 'groups', 'mixed') for s in (0, 0x80000000, 0xffffffff)},
            'Incomplete record matrix')
    for row in cases:
        validate_case(row)
    controls = corruption_controls(cases)
    paths = sorted(p for p in root.rglob('*') if p.is_file() and p.resolve() != args.out.resolve())
    sources = [Path('tools') / p for p in ('probe_original_debugger_records.py', 'probe_original_debugger_wire.py',
               'validate_debugger_records.py', 'ghidra/Invoke-DebuggerRecordResearch.ps1', 'ghidra/ExportFunctionEvidence.java')]
    ledger = dict(schema=1, scope=report['scope'], engine_recovered=False, shader_debug_parity=False,
                  nonempty_cases=len(cases), empty_reuse_cases=len(cases), rejected_corruptions=controls,
                  original_binaries=report['binaries'], evidence=[dict(path=p.as_posix(), bytes=p.stat().st_size,
                  sha256=digest(p)) for p in paths], sources=[dict(path=p.as_posix(), sha256=digest(p)) for p in sources])
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(ledger, indent=2) + '\n', encoding='utf-8')
    print(f'{len(cases)} record cases, {len(cases)} reuse cases, {controls} corrupted records rejected')


if __name__ == '__main__':
    main()
