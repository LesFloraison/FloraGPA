"""Check private PS completion observations and exact DXBC transformation boundaries."""
import argparse
import copy
import hashlib
import json
import struct
from pathlib import Path


def require(value, message):
    if not value:
        raise ValueError(message)


def chunks(data):
    require(data[:4] == b'DXBC' and len(data) >= 32, 'DXBC header')
    require(struct.unpack_from('<I', data, 24)[0] == len(data), 'DXBC size')
    count = struct.unpack_from('<I', data, 28)[0]
    require(count <= (len(data) - 32) // 4, 'DXBC chunk table')
    result = {}
    for offset in struct.unpack_from(f'<{count}I', data, 32):
        require(offset >= 32 + count * 4 and offset + 8 <= len(data), 'DXBC chunk offset')
        tag, size = struct.unpack_from('<4sI', data, offset)
        require(tag not in result and offset + 8 + size <= len(data), 'DXBC chunk bounds')
        result[tag] = data[offset + 8:offset + 8 + size]
    return result


def instructions(code):
    require(len(code) >= 8 and len(code) % 4 == 0, 'Shader code size')
    words = struct.unpack(f'<{len(code) // 4}I', code)
    require(words[1] == len(words), 'Shader token count')
    rows, pos = [], 2
    while pos < len(words):
        n = words[pos + 1] if words[pos] & 2047 == 53 else (words[pos] >> 24) & 127
        require(n > 0 and pos + n <= len(words), 'Instruction framing')
        rows.append(list(words[pos:pos + n]))
        pos += n
    return words[0], rows


def shader_identity(r, marker=(0x3f800000, 0x40000000, 0x40400000)):
    require(r['schema'] == 1 and r['backend'] == 'native-pixel-completion/1' and
            r['kind'] == 'native_pixel_completion_shader', 'Completion shader version')
    require(r['original_fragment_eligibility'] is None and r['alpha_preserved'] is True and
            r['requires_private_unblended_target'] is True, 'Completion shader scope')
    require(all(r[k] == 0 for k in ('extra_uav_slots', 'extra_input_registers', 'extra_temps')), 'Added resources')
    require(0 <= r['target'] < 8 and r['component_type'] in (1, 2, 3) and
            r['marker_returns'] > 0 and 0 < r['original_output_mask'] <= 15, 'Marker output identity')
    require(r['marker_bits'] == list(marker), 'Marker payload')


def shader_bytes(r, source, marked, marker=(0x3f800000, 0x40000000, 0x40400000)):
    shader_identity(r, marker)
    require(hashlib.sha256(source).hexdigest() == r['source_sha256'] and
            hashlib.sha256(marked).hexdigest() == r['shader_sha256'], 'Shader hashes')
    a, b = chunks(source), chunks(marked)
    code = next(k for k in a if k in (b'SHDR', b'SHEX'))
    version, rows = instructions(a[code])
    other_version, other_rows = instructions(b[code])
    require(version == other_version and version in (0x40, 0x41, 0x50), 'Preserved shader profile')
    declarations = [row for row in rows if row[0] & 2047 == 101 and len(row) >= 3 and
                    (row[1] >> 12) & 255 == 2 and row[-1] == r['target']]
    require(len(declarations) == 1, 'Unique selected output declaration')
    destination = declarations[0][1:]
    destination[0] = (destination[0] & ~0xf0) | 0x70
    marker = [((6 + len(destination)) << 24) | 54, *destination, 0x4002, *r['marker_bits'], 0]
    expected, main, added = [], True, 0
    for row in rows:
        opcode = row[0] & 2047
        if opcode == 44:
            main = False
        if opcode == 101 and len(row) >= 3 and row[-1] == r['target'] and (row[1] >> 12) & 255 == 2:
            row = row.copy()
            row[1] |= 7 << 4
        if main and opcode in (62, 63):
            expected.append(marker)
            added += 1
        expected.append(row)
    require(expected == other_rows and added == r['marker_returns'], 'Original executable instructions changed')
    stale = {b'STAT', b'SDBG', b'SPDB', b'ILDB', b'SRCI', b'ILDN'}
    require(set(b) == set(a) - stale, 'Unexpected added or removed shader chunks')
    for tag, original in a.items():
        if tag in stale or tag == code:
            continue
        expected = bytearray(original)
        if tag in (b'OSGN', b'OSG5', b'OSG1'):
            count = struct.unpack_from('<I', original)[0]
            stride = {b'OSGN': 24, b'OSG5': 28, b'OSG1': 32}[tag]
            for i in range(count):
                base = 8 + i * stride + (0 if tag == b'OSGN' else 4)
                if struct.unpack_from('<I', original, base + 16)[0] == r['target']:
                    expected[base + 20] |= 7
                    expected[base + 21] &= 0xf8
        require(bytes(expected) == b[tag], 'Non-diagnostic signature/resource/debug chunk change')


def observation(r):
    require(r['schema'] == 1 and r['kind'] == 'native_pixel_completion_observation' and
            r['backend'] == 'native-pixel-completion-observation/1', 'Completion observation version')
    require(r['scope'] == 'original_ps_body_on_private_completion_target' and
            r['original_fragment_eligibility'] is None and r['original_gpu_schedule'] is False and
            r['diagnostic_shader_replacements'] is True and r['extra_uav_slots'] == 0, 'Completion observation scope')
    shader_identity(r['completion_shader'])
    require(r['shaders'][4]['sha256'] == r['completion_shader']['source_sha256'], 'Bound original shader')
    target = r['completion_target']
    require(target['slot'] == r['completion_shader']['target'] and target['cleared_to_zero'] is True and
            target['blend_and_logic_disabled'] is True and target['write_mask'] == 15, 'Private target overrides')
    require(r['debug_validation'] == 'no_errors' and r['count_domain'] == 'entire_bound_output' and
            type(r['passing_samples']) is int and r['passing_samples'] >= 0, 'Observed query scope')


def changed_payloads(data):
    """Mutate actual code/signature bytes while retaining parseable container framing."""
    count = struct.unpack_from('<I', data, 28)[0]
    for offset in struct.unpack_from(f'<{count}I', data, 32):
        tag, size = struct.unpack_from('<4sI', data, offset)
        pos = offset + 8
        if tag in (b'SHDR', b'SHEX'):
            _, rows = instructions(data[pos:pos + size])
            last = pos + 8 + sum(len(row) * 4 for row in rows[:-1])
            bad = bytearray(data)
            bad[last] ^= 1
            yield 'executable', bytes(bad)
        elif tag in (b'OSGN', b'OSG5', b'OSG1'):
            bad = bytearray(data)
            bad[pos + 8 + (0 if tag == b'OSGN' else 4) + 20] ^= 1
            yield 'output_signature', bytes(bad)


def owned(r):
    if r['kind'] == 'native_pixel_completion_observation':
        observation(r)
        require(r['recovered_failures'] == 8 and r['original_binding_draw_matches'] is True and
                r['original_storage_restored'] is True, 'Recovery and original binding check')
        return r['completion_shader']
    shader_identity(r)
    require(r['status'] == 'ok' and r['original_storage_restored'] is True, 'Owned result')
    o = r['private_observation']
    observation(o)
    require(o['completion_shader']['source_sha256'] == r['source_sha256'], 'Private shader join')
    if 'merger' in r:
        m = r['merger']
        require(m['marked'][:3] == r['marker_bits'] and m['cleared'] == [0, 0, 0, 0], 'Merger completion marker')
        require(m['other'] == m['reference_other'], 'Other target preservation')
        require(o['passing_samples'] == r['diagnostic_passing_samples'] and
                r['reference_passing_samples'] == r['direct_native_passing_samples'], 'Independent merger queries')
        require(r['single_sample_query_contract_match'] is (r['reference_passing_samples'] == 1 and r['diagnostic_passing_samples'] == 1) and
                r['diagnostic_query_matches_original'] is (r['diagnostic_passing_samples'] == r['reference_passing_samples']), 'Merger query deviation disclosure')
        require(m['original_value_unchanged'] is (m['before'] == m['reference']) and
                m['original_value_unchanged'] is (r['mode'] in ('logic', 'write-mask', 'same-value')), 'Same-value diagnostic scope')
        require(m['marked'][3] == (0x80000001 if r['mode'] == 'logic' else 0x3e800000 if r['mode'] == 'same-value' else 0x3f000000), 'Original merger alpha')
    elif 'values' in r:
        count = 0
        require(len(r['values']) == len(o['selected_samples']) == r['samples'], 'Sample coverage')
        for index, row in enumerate(r['values']):
            require(row['sample'] == index, 'Sample identity')
            private = o['selected_samples'][index]
            if row['marker_survived']:
                require(row['marked'][:3] == private[:3] == r['marker_bits'], 'Marker RGB')
                if r['mode'] != 'partial':
                    require(row['marked'][3] == private[3] == row['reference'][3], 'Preserved alpha')
                count += 1
            else:
                require(row['marked'] == row['before'] and private == [0, 0, 0, 0], 'Discarded/masked marker')
        require(count == r['marker_samples'] and o['passing_samples'] == r['reference_passing_samples'], 'Marker/query counts')
    elif 'reference_depth' in r:
        require(r['reference_depth'] == r['marked_depth'] == r['expected_depth'] == o['selected_depth'], 'Depth/stencil preservation')
        require(r['direct_native_passing_samples'] == r['reference_passing_samples'] == o['passing_samples'], 'Independent native query')
        if r['early_occlusion_contract_match'] is not None:
            require(r['early_occlusion_contract_match'] is (r['reference_passing_samples'] == 1), 'Early query deviation disclosure')
        if r['marker_survived']:
            require(list(struct.unpack('<4I', bytes(r['marked_color'])))[:3] == r['marker_bits'] and
                    o['selected_color'] == r['marked_color'] and r['marked_color'][12:] == r['reference_color'][12:], 'Depth-tested marker')
        else:
            require(r['marked_color'] == r['reference_color'] and o['selected_color'] == [0] * 16, 'Rejected marker storage')
    else:
        require(r['reference_buffer'] == r['marked_buffer'] == o['uav_storage'] and
                r['counter'] == o['hidden_counter'] and r['marked_color'] == o['selected_color'], 'UAV/class preservation')
    return r


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('owned', type=Path)
    parser.add_argument('--real', type=Path)
    parser.add_argument('--repeat', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    paths = sorted(args.owned.glob('*.json'))
    require(len(paths) == 126, 'Complete owned evidence matrix')
    owned_count = len(paths)
    repeated = sorted(args.repeat.glob('*/*.json')) if args.repeat else []
    if args.repeat:
        require(len(repeated) == 12, 'Complete dual-source repetition matrix')
        for path in repeated:
            r = json.loads(path.read_text(encoding='utf-8'))
            require(r['mode'] == 'dual-source' and r['vendor_request'] == 32902, 'Repeated fixture identity')
    paths += repeated
    results, deviations, merger_deviations, negatives = [], [], [], 0
    for path in paths:
        r = json.loads(path.read_text(encoding='utf-8'))
        s = owned(r)
        if r.get('early_occlusion_contract_match') is False:
            deviations.append(str(path))
        if r.get('single_sample_query_contract_match') is False:
            merger_deviations.append(str(path))
        if r['kind'] != 'native_pixel_completion_observation':
            source = Path(str(path) + '.original.dxbc').read_bytes()
            marked = Path(str(path) + '.marked.dxbc').read_bytes()
            shader_bytes(s, source, marked)
            for key, value in [('original_fragment_eligibility', True), ('extra_uav_slots', 1), ('alpha_preserved', False), ('source_sha256', '0' * 64)]:
                bad = copy.deepcopy(s)
                bad[key] = value
                try:
                    shader_bytes(bad, source, marked)
                except ValueError:
                    negatives += 1
                    continue
                raise ValueError('Accepted corrupted shader identity: ' + key)
            for key, payload in changed_payloads(marked):
                bad = copy.deepcopy(s)
                bad['shader_sha256'] = hashlib.sha256(payload).hexdigest()
                try:
                    shader_bytes(bad, source, payload)
                except ValueError:
                    negatives += 1
                    continue
                raise ValueError('Accepted changed shader payload: ' + key)
        results.append({'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    real = []
    if args.real:
        real = sorted(args.real.glob('*/report.json'))
        require(len(real) == 9, 'Complete real evidence matrix')
        for path in real:
            r = json.loads(path.read_text(encoding='utf-8'))
            observation(r)
            shader_bytes(r['completion_shader'], (path.parent / 'original.dxbc').read_bytes(), (path.parent / 'marked.dxbc').read_bytes())
            require(r['probe']['status'] == 'ok' and r['probe']['original_storage_restored'] is True, 'Real replay validation')
            require((path.parent / 'before.bin').read_bytes() == (path.parent / 'restored.bin').read_bytes(), 'Real original storage')
            require((path.parent / 'original-draw.bin').read_bytes() == (path.parent / 'reference.bin').read_bytes(), 'Independent real replay')
            require(r['passing_samples'] == r['original_draw']['passing_samples'] == r['probe']['reference_passing_samples'], 'Real query equality')
            require(r['probe']['marker_changed_storage'] is ((path.parent / 'cleared.bin').read_bytes() != (path.parent / 'marked.bin').read_bytes()), 'Real marker change')
            results.append({'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    result = {'schema': 1, 'status': 'ok', 'owned_cases': owned_count, 'repeated_cases': len(repeated), 'real_cases': len(real), 'negative_controls': negatives,
              'early_query_contract_deviations': deviations, 'merger_query_contract_deviations': merger_deviations,
              'complete_pixel_history': False, 'files': results}
    with args.out.open('x', encoding='utf-8') as out:
        json.dump(result, out, indent=2)
        out.write('\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'files'}))


if __name__ == '__main__':
    main()
