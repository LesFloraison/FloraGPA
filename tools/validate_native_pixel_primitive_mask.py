"""Validate primitive predicates, original DXBC preservation and exact owned GPU observations."""
import argparse
import copy
import hashlib
import json
import struct
from pathlib import Path
from validate_native_pixel_completion import chunks, instructions, require, changed_payloads


def signature(data, tag):
    count = struct.unpack_from('<I', data)[0]
    stride = 32 if tag in (b'ISG1', b'OSG1') else 28 if tag == b'OSG5' else 24
    bias = 0 if stride == 24 else 4
    require(8 + count * stride <= len(data), 'Signature framing')
    rows = []
    for i in range(count):
        offset = 8 + i * stride + bias
        name, index, system, kind, reg, mask = struct.unpack_from('<6I', data, offset)
        require(name >= 8 + count * stride and name < len(data), 'Semantic bounds')
        end = data.index(0, name)
        rows.append([data[name:end].decode('ascii').lower(), index, system, kind, reg,
                     mask & 255, (mask >> 8) & 255,
                     struct.unpack_from('<I', data, offset + 24)[0] if stride == 32 else 0])
    return rows


def shader(r, source, marked):
    complement = r.get('primitive_comparison', 'equal') == 'not_equal'
    require(r.get('primitive_comparison', 'equal') in ('equal', 'not_equal'), 'Predicate comparison')
    require(r['schema'] == 1 and r['kind'] == 'native_pixel_primitive_mask_shader' and
            r['backend'] == ('native-pixel-primitive-mask/2' if complement else
                             'native-pixel-primitive-mask/1'), 'Shader schema')
    require(r['source_sha256'] == hashlib.sha256(source).hexdigest() and
            r['shader_sha256'] == hashlib.sha256(marked).hexdigest(), 'Shader hashes')
    require(r['alpha_preserved'] is True and r['extra_uav_slots'] == 0 and r['extra_temps'] == 1 and
            r['original_fragment_eligibility'] is None and r['requires_primitive_id_producer'] is True and
            r['nonmatching_rgb_zero'] is True, 'Predicate scope')
    require(r['marker_bits'] == [0x3f800001, 0x40000001, 0x40400001], 'Marker literals')
    mask, value, reg, component, temp = [r[k] for k in
        ('primitive_mask', 'primitive_value', 'input_register', 'input_component', 'temporary_register')]
    require(0 < mask <= 0xffffffff and value & ~mask == 0 and 0 <= reg < 32 and 0 <= component < 4,
            'Primitive predicate')
    a, b = chunks(source), chunks(marked)
    code = next(k for k in a if k in (b'SHDR', b'SHEX'))
    inputs = next(k for k in a if k in (b'ISGN', b'ISG1'))
    old_inputs, new_inputs = signature(a[inputs], inputs), signature(b[inputs], inputs)
    existing = [row for row in old_inputs if row[2] == 7]
    require(len(existing) <= 1, 'Original primitive semantic')
    expected_inputs = copy.deepcopy(old_inputs)
    if existing:
        row = next(row for row in expected_inputs if row[2] == 7)
        require(row[3:6] == [1, reg, 1 << component], 'Existing primitive input')
        row[6] |= 1 << component
    else:
        expected_inputs.append(['sv_primitiveid', 0, 7, 1, reg, 1 << component, 1 << component, 0])
    require(expected_inputs == new_inputs, 'Preserved original input signature')
    require(r['extra_input_components'] == (0 if existing else 1) and
            r['extra_input_registers'] == (0 if existing or reg in [row[4] for row in old_inputs] else 1),
            'Added input accounting')
    version, original = instructions(a[code])
    other_version, observed = instructions(b[code])
    require(version == other_version, 'Original shader profile')
    outputs = [row for row in original if row[0] & 2047 == 101 and row[-1] == r['target']]
    require(len(outputs) == 1, 'Selected output declaration')
    destination = outputs[0][1:]
    destination[0] = (destination[0] & ~0xf0) | 0x70
    temps = [row for row in original if row[0] & 2047 == 104]
    require(len(temps) <= 1 and temp == (temps[0][1] if temps else 0), 'Fresh temporary')
    and_row = [0x07000001, 0x00100012, temp, 0x0010100a | (component << 4), reg, 0x4001, mask]
    eq_row = [0x07000027 if complement else 0x07000020, 0x00100012, temp, 0x0010000a, temp, 0x4001, value]
    select_row = [((13 + len(destination)) << 24) | 55, *destination,
                  0x0010000a, temp, 0x4002, *r['marker_bits'], 0, 0x4002, 0, 0, 0, 0]
    restored, index, returns, input_decls, temp_decls, main = [], 0, 0, 0, 0, True
    while index < len(observed):
        row = observed[index].copy()
        op = row[0] & 2047
        if op == 44:
            main = False
        if main and row == and_row:
            require(observed[index + 1:index + 3] == [eq_row, select_row] and
                    observed[index + 3][0] & 2047 in (62, 63), 'Conditional return marker')
            index += 3
            returns += 1
            continue
        index += 1
        if not existing and row == [0x04000063, 0x00101002 | (1 << (component + 4)), reg, 7]:
            input_decls += 1
            continue
        if op == 104:
            require(row == [0x02000068, temp + 1], 'Temporary declaration')
            temp_decls += 1
            if not temps:
                continue
            row[1] -= 1
        if op == 101 and row[-1] == r['target']:
            row[1] = (row[1] & ~0xf0) | (outputs[0][1] & 0xf0)
        restored.append(row)
    require(restored == original and returns == r['marker_returns'] and returns > 0 and
            input_decls == (0 if existing else 1) and temp_decls == 1, 'Original executable preservation')
    stale = {b'STAT', b'SDBG', b'SPDB', b'ILDB', b'SRCI', b'ILDN'}
    require(set(b) == set(a) - stale, 'Shader chunks')
    for tag in set(b) - {code, inputs}:
        if tag in (b'OSGN', b'OSG5', b'OSG1'):
            expected = signature(a[tag], tag)
            for row in expected:
                if row[4] == r['target']:
                    row[5] |= 7
                    row[6] &= ~7
            require(expected == signature(b[tag], tag), 'Preserved output signature')
        else:
            require(a[tag] == b[tag], 'Original resource/other chunk')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--real', type=Path)
    args = parser.parse_args()
    paths = sorted(args.directory.glob('*.json'))
    require(len(paths) in (27,30), 'Complete adapter and mode matrix')
    passes, negative, files, seen = 0, 0, [], set()
    for path in paths:
        r = json.loads(path.read_text(encoding='utf-8'))
        require(r['status'] == 'ok' and r['original_storage_restored'] is True and
                r['original_fragment_eligibility'] is None, 'Owned observation scope')
        mode, vendor = r['mode'], r['vendor_request']
        require(mode in ('added', 'existing', 'discard', 'early-discard', 'integer', 'sample', 'upper-gs', 'added-gs', 'duplicate-gs', 'packed'), 'Owned mode')
        require(vendor in (0, 4318, 32902) and (vendor, mode) not in seen, 'Unique adapter/mode')
        seen.add((vendor, mode))
        expected = 7 if mode == 'duplicate-gs' else 0xfffffffe if mode.endswith('gs') else 1 if 'discard' in mode else 2
        require(r['expected_system_primitive_id'] == expected and len(r['observations']) in (34, 36),
                'Authored expected identity and all predicates')
        source = Path(str(path) + '.original.dxbc').read_bytes()
        for index, row in enumerate(r['observations']):
            s = row['completion_shader']
            marked = Path(str(path) + f'.{index}.dxbc').read_bytes()
            shader(s, source, marked)
            mask = 1 << index if index < 32 else 0xffffffff
            value = mask if index < 32 else expected ^ 1 if index == 33 else expected ^ 2 if index == 35 else expected
            complement = index >= 34
            require(s.get('primitive_comparison', 'equal') == ('not_equal' if complement else 'equal'),
                    'Owned comparison mode')
            require(s['primitive_mask'] == mask and s['primitive_value'] == value, 'Predicate selection')
            require(row['kind'] == 'native_pixel_primitive_mask_observation' and
                    row['backend'] == ('native-pixel-primitive-mask-observation/2' if complement else
                                       'native-pixel-primitive-mask-observation/1') and
                    row['scope'] == 'original_ps_body_with_private_primitive_predicate' and
                    row['original_fragment_eligibility'] is None and row['primitive_ordinal'] is None and
                    row['instance'] is None and row['debug_validation'] == 'no_errors' and
                    row['original_gpu_schedule'] is False, 'GPU observation scope')
            require(row['primitive_id_source'] == ('bound_geometry_shader' if mode.endswith('gs') else 'rasterizer'),
                    'Observed system identity producer')
            require(row['event'] == 100 and row['shaders'][4]['sha256'] == s['source_sha256'] and
                    row['adapter']['vendor'] == (5140 if vendor == 0 else vendor), 'Actual binding and adapter')
            require(len(row['pixels']) == r['samples'] == (4 if mode == 'sample' else 1), 'Samples')
            for sample, pixel in enumerate(row['pixels']):
                require(pixel[:3] == (s['marker_bits'] if ((expected & mask == value) != complement) else [0, 0, 0]) and
                        pixel[3] == r['reference'][sample][3], 'Exact predicate output and alpha')
            mutations = [('primitive_mask', mask ^ 1), ('input_component', s['input_component'] ^ 1),
                         ('extra_uav_slots', 1), ('original_fragment_eligibility', True)]
            for key, value in mutations:
                bad = copy.deepcopy(s)
                bad[key] = value
                try:
                    shader(bad, source, marked)
                except (ValueError, IndexError):
                    negative += 1
                    continue
                raise ValueError('Accepted identity corruption: ' + key)
            for key, payload in changed_payloads(marked):
                bad = copy.deepcopy(s)
                bad['shader_sha256'] = hashlib.sha256(payload).hexdigest()
                try:
                    shader(bad, source, payload)
                except (ValueError, IndexError):
                    negative += 1
                    continue
                raise ValueError('Accepted changed shader: ' + key)
            passes += 1
        files.append({'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    modes = {'added', 'existing', 'discard', 'early-discard', 'integer', 'sample', 'upper-gs', 'added-gs', 'packed'}
    if len(paths) == 30:
        modes.add('duplicate-gs')
    require(seen == {(vendor,mode) for vendor in (0,4318,32902) for mode in modes}, 'Exact adapter/mode coverage')
    real = sorted(args.real.glob('*/report.json')) if args.real else []
    if args.real:
        require(len(real) == 9, 'Complete real capture matrix')
        for path in real:
            r = json.loads(path.read_text(encoding='utf-8'))
            shader(r['completion_shader'], (path.parent / 'original.dxbc').read_bytes(),
                   (path.parent / 'marked.dxbc').read_bytes())
            require(r['kind'] == 'native_pixel_primitive_mask_observation' and
                    r['scope'] == 'original_ps_body_with_private_primitive_predicate' and
                    r['original_fragment_eligibility'] is None and r['primitive_ordinal'] is None and
                    r['instance'] is None and r['debug_validation'] == 'no_errors' and
                    r['probe']['status'] == 'ok' and r['probe']['original_storage_restored'] is True,
                    'Real primitive predicate scope and replay')
            require((path.parent / 'before.bin').read_bytes() == (path.parent / 'restored.bin').read_bytes() and
                    (path.parent / 'original-draw.bin').read_bytes() == (path.parent / 'reference.bin').read_bytes(),
                    'Real original and independent storage')
            require(r['passing_samples'] == r['original_draw']['passing_samples'] == r['probe']['reference_passing_samples'],
                    'Real query comparisons')
            require(r['probe']['primitive_predicate_matched'] is None and
                    r['probe']['diagnostic_storage_changed'] is ((path.parent / 'cleared.bin').read_bytes() != (path.parent / 'marked.bin').read_bytes()),
                    'Real storage change observation')
            files.append({'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    result = {'schema': 1, 'status': 'ok', 'cases': len(paths), 'predicate_observations': passes, 'real_cases': len(real),
              'negative_controls': negative, 'complete_pixel_history': False, 'files': files}
    with args.out.open('x', encoding='utf-8') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(json.dumps({k:v for k,v in result.items() if k != 'files'}))


if __name__ == '__main__':
    main()
