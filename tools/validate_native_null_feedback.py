"""Audit discarded feedback data without waiving original GPU shader failures."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path
import re
import struct

from validate_feedback_relocation import instructions, require


def first_operand(row):
    position = 1
    while row[position - 1] >> 31:
        position += 1
    return row[position]


def buffer_result(report):
    fields = report['fields']
    require(fields in (2, 3, 4) and report['vendor'] in (0, 4318, 32902), 'Buffer identity')
    expected = [100 + 4 * fields + c for c in range(3)] + [1]
    require(report['null_loads'] == fields - 1 and report['loads'] == fields and
            report['merges'] == fields - 1, 'Buffer instruction counts')
    require(report['gpu'] == report['control'] == report['expected'] == expected, 'Buffer oracle')
    require(len(report['native']) == 2, 'Native buffer cases')
    for row, known in zip(report['native'], (False, True)):
        require(row['known_mapping'] is known and row['words'][:3] == expected[:3], 'Native buffer data')
        require(row['validity'] == [1] * 12 + [int(known)] * 4, 'Native buffer validity')
        if known:
            require(row['words'] == expected, 'Native buffer mapping')


def texture_result(report, key):
    vendor, stage, mode, known = key
    require(report['stage'] == stage and report['mode'] == mode and
            report['known_non_tiled'] is bool(known), 'Texture identity')
    require(report['adapter']['vendor_id'] == (vendor or 5140) and
            report['adapter']['warp'] is (vendor == 0), 'Actual texture adapter')
    values = ([10 / 16] * 4, [10 / 16] * 4, [1] * 4,
              [10 / 16, 11 / 16, 7 / 16, 6 / 16],
              [11 / 16, 12 / 16, 8 / 16, 7 / 16], [1, 1, 0, 0], [1, 1, 1, 0])[mode]
    expected = [struct.unpack('<I', struct.pack('<f', value))[0] for value in values]
    require(report['observed_feedback'] == report['observed_control'] == expected, 'Observed data oracle')
    constant = [99, 123, 456, 0]
    require(report['constant_control'] == constant, 'Constant GPU oracle')
    require(report['valid'] == (15 if known else 7), 'Native texture validity')
    require(all(report['native'][c] == constant[c] for c in range(4)
                if report['valid'] & (1 << c)), 'Native texture output')
    require(report['opaque_changes'] > 0, 'Missing opaque feedback')


def creation_result(row):
    require(row['stage'] in (0, 1, 5) and row['mode'] in (1, 2), 'Creation identity')
    require(row['data_observed'] == dict(creation_hresult=0, removed_reason=0), 'Creation positive control')
    for key in ('original', 'canonical'):
        result = row[key]
        if result['creation_hresult'] == 0:
            require(result['removed_reason'] == 0, 'Removed successful creation device')
        else:
            require(result == dict(creation_hresult=0x887A0005, removed_reason=0x887A0020),
                    'Unclassified shader creation failure')
    require(row['canonical'] == row['original'], 'Padding changed creation outcome')


def rejected(check, report, mutations):
    count = 0
    for mutation in mutations:
        altered = copy.deepcopy(report)
        mutation(altered)
        try:
            check(altered)
        except ValueError:
            count += 1
        else:
            raise ValueError('Corrupted observation accepted')
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--buffers', type=Path, required=True)
    parser.add_argument('--textures', type=Path, required=True)
    parser.add_argument('--creation', type=Path, required=True)
    parser.add_argument('--texture-log', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    corruptions, buffer_keys, hashes = 0, set(), {}
    paths = list(args.buffers.glob('*/report.json'))
    require(len(paths) == 9, 'Nine buffer cases')
    for path in paths:
        report = json.loads(path.read_text())
        buffer_result(report)
        buffer_keys.add((report['vendor'], report['fields']))
        for name, digest, merges, checks in (
                ('shader', report['shader_sha256'], report['fields'] - 1, 1),
                ('original', report['original_sha256'], 0, report['fields'])):
            shader = path.with_name(name + '.dxbc')
            require(hashlib.sha256(shader.read_bytes()).hexdigest() == digest, 'Buffer shader digest')
            code = instructions(shader)
            loads = [row for row in code if row[0] & 2047 == 227]
            require(len(loads) == report['fields'] and
                    sum(first_operand(row) == 0xD000 for row in loads) == report['fields'] - 1 and
                    sum(row[0] & 2047 == 60 for row in code) == merges and
                    sum(row[0] & 2047 == 234 for row in code) == checks, 'Buffer opcode framing')
        corruptions += rejected(buffer_result, report, [
            lambda r: r['gpu'].__setitem__(0, 0),
            lambda r: r['control'].__setitem__(3, 0),
            lambda r: r.__setitem__('null_loads', 0),
            lambda r: r['native'][0]['validity'].__setitem__(12, 1),
            lambda r: r['native'][1]['validity'].__setitem__(12, 0)])
    require(buffer_keys == set(itertools.product((0, 4318, 32902), (2, 3, 4))), 'Buffer matrix')
    creation = json.loads((args.creation / 'creation.json').read_text())
    require(len(creation) == 6 and {(r['stage'], r['mode']) for r in creation} ==
            set(itertools.product((0, 1, 5), (1, 2))), 'Creation matrix')
    blocked = set()
    for row in creation:
        creation_result(row)
        folder = args.creation / f"{row['stage']}-{row['mode']}"
        for kind in ('original', 'canonical', 'observed'):
            data = (folder / f'{kind}.dxbc').read_bytes()
            require(hashlib.sha256(data).hexdigest() == row[f'{kind}_sha256'], 'Creation shader digest')
        original, canonical = instructions(folder / 'original.dxbc'), instructions(folder / 'canonical.dxbc')
        require(len(original) == len(canonical), 'Canonical instruction count')
        for before, after in zip(original, canonical):
            expected = before.copy()
            if expected[0] & 2047 in (228, 229):
                require(first_operand(expected) == 0xD000 and expected[-1] == 0, 'Canonical null sample')
                expected.pop()
                expected[0] = (expected[0] & ~(127 << 24)) | len(expected) << 24
            require(expected == after, 'Only compiler padding may change')
        if row['original']['creation_hresult']:
            blocked.update(f"32902-{row['stage']}-{row['mode']}-{known}" for known in (0, 1))
        corruptions += rejected(creation_result, row, [
            lambda r: r['data_observed'].__setitem__('creation_hresult', 1),
            lambda r: r['canonical'].__setitem__('removed_reason', 1)])
    matrix = list(itertools.product((0, 4318, 32902), (0, 1, 5), range(7), (0, 1)))
    names = {'-'.join(map(str, key)) for key in matrix}
    require({p.name for p in args.textures.iterdir() if p.is_dir()} == names, 'Texture matrix')
    complete = set()
    for key in matrix:
        vendor, stage, mode, known = key
        name = '-'.join(map(str, key))
        folder = args.textures / name
        report = json.loads((folder / 'native.json').read_text())
        texture_result(report, key)
        corruptions += rejected(lambda r: texture_result(r, key), report, [
            lambda r: r['native'].__setitem__(0, 0),
            lambda r: r['observed_feedback'].__setitem__(0, 0),
            lambda r: r['constant_control'].__setitem__(3, 1),
            lambda r: r.__setitem__('valid', r['valid'] ^ 8),
            lambda r: r.__setitem__('opaque_changes', 0)])
        for kind in ('feedback', 'control', 'observed-feedback', 'observed-control'):
            shader = folder / f'{kind}.dxbc'
            digest = hashlib.sha256(shader.read_bytes()).hexdigest()
            require(hashes.setdefault((stage, mode, kind), digest) == digest, 'Identical matrix shaders')
            code = instructions(shader)
            feedback = [row for row in code if 219 <= row[0] & 2047 <= 233]
            if 'feedback' in kind:
                require(len(feedback) == 1 and feedback[0][0] & 2047 == (223, 228, 229, 219, 221, 220, 222)[mode],
                        'Feedback opcode')
                require((first_operand(feedback[0]) == 0xD000) == (kind == 'feedback'), 'Data destination')
            else:
                require(not feedback, 'Ordinary control has feedback')
        final = folder / 'report.json'
        if final.exists():
            data = json.loads(final.read_text())
            require(data['null_data_destination'] and data['original'] == data['control'] == [99, 123, 456, 0],
                    'Original null shader oracle')
            complete.add(name)
    require(complete == names - blocked, 'Every original result or failure must be accounted for')
    log = args.texture_log.read_text(encoding='utf-8-sig')
    failures = re.findall(r'^FAIL!.*::nullTextureData\(([^)]+)\) (.*)$', log, re.MULTILINE)
    require(len(failures) == len(blocked) == log.count('FAIL!') and {name for name, _ in failures} == blocked,
            'Qt failures must match independent creation evidence')
    require(all('Create original precision' in text and 'HRESULT 0x887a0005' in text for _, text in failures),
            'Unexpected Qt failure reason')
    result = dict(schema=1, status='blocked-by-original-shader-creation' if blocked else 'verified',
                  buffer_gpu_cases=9, buffer_native_cases=18, texture_native_cases=126,
                  texture_data_observed_controls=126, original_null_texture_results=len(complete),
                  original_null_texture_failures=sorted(blocked), isolated_creation_variants=18,
                  corruptions_rejected=corruptions, full_debugger_parity=False,
                  tiled_mapping_verified=False)
    args.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))
    return int(bool(blocked))


if __name__ == '__main__':
    raise SystemExit(main())
