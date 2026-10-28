"""Validate original-draw observation evidence without running a graphics backend."""
import argparse
import copy
import hashlib
import json
import re
import struct
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unsigned(value):
    return type(value) is int and 0 <= value <= (1 << 64) - 1


def digest(value):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) is not None


def equal(a, b):
    require(a['typed_format'] == b['typed_format'] and a['valid_bytes'] == b['valid_bytes'] and
            len(a['bytes']) == len(b['bytes']) == len(a['valid_bytes']), 'Pixel storage shape')
    return all(x == y for x, y, valid in zip(a['bytes'], b['bytes'], a['valid_bytes']) if valid)


def observation(report):
    return report if report.get('kind') == 'native_original_draw_observation' else report.get('original_draw_observation')


def validate(report):
    r = observation(report)
    require(r is not None, 'Missing original draw')
    require(r['schema'] == 1 and r['kind'] == 'native_original_draw_observation' and
            r['backend'] == 'native-original-draw/1', 'Observation version')
    require(r['scope'] == 'original_draw_on_private_outputs' and
            r['count_domain'] == 'entire_bound_output', 'Observation domain')
    require(r['original_fragment_eligibility'] is None and r['original_gpu_schedule'] is False and
            r['diagnostic_shader_replacements'] is False and r['extra_uav_slots'] == 0, 'Evidence scope')
    require(digest(r['frame_sha256']) and unsigned(r['event']) and r['event'] > 0, 'Frame/event identity')
    require(0x37 <= r['command_type'] <= 0x3d and type(r['enabled']) is bool, 'Draw command')
    require(unsigned(r['private_output_payload_bytes']) and unsigned(r['payload_limit']) and
            0 < r['payload_limit'] and r['private_output_payload_bytes'] <= r['payload_limit'], 'Payload bound')
    require(len(r['shaders']) == 5, 'Graphics stages')
    for stage, shader in enumerate(r['shaders']):
        require(shader['stage'] == stage and unsigned(shader['resource']) and
                (digest(shader['sha256']) if shader['resource'] else shader['sha256'] is None), 'Shader identity')
    keys = {'ia_vertices', 'ia_primitives', 'vs_invocations', 'hs_invocations', 'ds_invocations',
            'gs_invocations', 'gs_primitives', 'clip_invocations', 'clip_primitives', 'ps_invocations', 'cs_invocations'}
    require(set(r['pipeline_statistics']) == keys and all(unsigned(v) for v in r['pipeline_statistics'].values()),
            'Pipeline counts')
    require(unsigned(r['passing_samples']) and r['pipeline_statistics']['cs_invocations'] == 0, 'Draw-only counts')
    if not r['enabled']:
        require(r['passing_samples'] == 0 and not any(r['pipeline_statistics'].values()), 'Disabled draw executed')
    require(r['debug_validation'] == 'no_errors', 'D3D11 validation')
    adapter = r['adapter']
    require(type(adapter['warp']) is bool and adapter['vendor'] in (5140, 4318, 32902) and
            adapter['warp'] is (adapter['vendor'] == 5140), 'Explicit validated adapter')
    if 'original_draw_observation' in report:
        require(r['frame_sha256'] == report['frame_sha256'] and r['event'] == report['event'] and
                adapter == report['adapter'], 'Parent execution context')
    if 'geometry_observed_value_matches' in r:
        require(equal(report['before'], r['before']), 'Original draw starting storage')
        matched = equal(report['after'], r['after'])
        require(r['geometry_observed_value_matches'] is matched, 'Pixel comparison')
        exact = matched if report['after']['exact_storage_bits'] and r['after']['exact_storage_bits'] else None
        require(r['geometry_exact_storage_matches'] is exact, 'Pixel precision comparison')
        count = sum(batch['passing_samples'] for batch in report['geometry_replay']['batches'])
        require(r['geometry_passing_samples'] == count and
                r['geometry_passing_samples_match'] is (r['passing_samples'] == count), 'Query comparison')
        require(report['observed_storage_restored'] is True and
                report['original_storage_restored'] is (True if report['before']['exact_storage_bits'] else None),
                'Original storage restoration precision')
    if 'reference_prefixes' in report:
        require(r['passing_samples'] == report['reference_prefixes'][-1]['passing_samples'], 'Independent draw count')
    if 'reference_sha256' in report:
        require(report['output_sha256'] == report['reference_sha256'] and digest(report['output_sha256']),
                'Independent output digest')
    if 'original_draw_atomic_reference' in report:
        atomic = report['original_draw_atomic_reference']
        require(atomic['schema'] == 1 and atomic['allocation_order_assumed'] is False, 'Atomic ordering scope')
        mode = report['mode']
        require(mode in ('counter', 'append', 'raw', 'typed', 'texture'), 'Atomic fixture mode')
        for key in ('observed_color', 'reference_color', 'repeat_color'):
            color = atomic[key]
            require(len(color) == 4 and color[1:] == [11, 17, 23] and
                    (color[0] == 7 if mode == 'append' else type(color[0]) is int and 0 <= color[0] < 3),
                    'Atomic return constraints')
        require(atomic['observed_color'] == list(struct.unpack('<4I', bytes(r['after']['bytes']))),
                'Atomic observation bytes')
        expected = [11, 12, 13, 0] if mode == 'counter' else [7, 7, 7, 0] if mode == 'append' else [3, 0, 0, 0]
        storage = list(struct.pack('<4I', *expected))[:4 if mode == 'texture' else 16]
        require(atomic['reference_storage'] == atomic['repeat_storage'] == storage, 'Atomic storage constraints')
        require(atomic['counter'] == (3 if mode in ('counter', 'append') else None), 'Atomic counter')
        repeat = validate(atomic['repeat'])
        require(repeat['frame_sha256'] == r['frame_sha256'] and repeat['adapter'] == r['adapter'] and
                repeat['event'] == r['event'] and repeat['passing_samples'] == atomic['reference_passing_samples'] == 3,
                'Atomic repeat execution')
    return r


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directories', nargs='+', type=Path)
    parser.add_argument('--expected', required=True, type=int)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    rows, skipped = [], []
    for path in sorted({p for directory in args.directories for p in directory.rglob('*.json')}):
        raw = path.read_bytes()
        report = json.loads(raw)
        if observation(report) is None:
            skipped.append(str(path))
            continue
        r = validate(report)
        if 'probe' in report:
            require(report['probe']['status'] == 'ok', 'Real probe status')
            reference = (path.parent / 'reference.bin').read_bytes()
            require(hashlib.sha256(reference).hexdigest() == report['probe']['reference_sha256'] and
                    list(reference) == r['after']['bytes'] and r['after']['exact_storage_bits'] is True,
                    'Independent real draw storage')
        mutations = [('scope', 'original_capture'), ('original_fragment_eligibility', True),
                     ('diagnostic_shader_replacements', True), ('passing_samples', -1),
                     ('private_output_payload_bytes', r['payload_limit'] + 1), ('shaders', [])]
        if 'geometry_observed_value_matches' in r:
            mutations += [('geometry_observed_value_matches', not r['geometry_observed_value_matches']),
                          ('geometry_passing_samples_match', not r['geometry_passing_samples_match'])]
        for key, value in mutations:
            altered = copy.deepcopy(report)
            observation(altered)[key] = value
            try:
                validate(altered)
            except (ValueError, KeyError, TypeError):
                continue
            raise ValueError(f'Corruption accepted: {path}: {key}')
        atomic_controls = 0
        if 'original_draw_atomic_reference' in report:
            for key, value in [('allocation_order_assumed', True), ('reference_storage', []),
                               ('reference_color', [99, 11, 17, 23])]:
                altered = copy.deepcopy(report)
                altered['original_draw_atomic_reference'][key] = value
                try:
                    validate(altered)
                except (ValueError, KeyError, TypeError):
                    atomic_controls += 1
                    continue
                raise ValueError(f'Atomic corruption accepted: {path}: {key}')
        rows.append({'path': str(path), 'sha256': hashlib.sha256(raw).hexdigest(), 'adapter': r['adapter'],
                     'command_type': r['command_type'], 'negative_controls': len(mutations) + atomic_controls,
                     'pixel_match': r.get('geometry_observed_value_matches'),
                     'sample_count_match': r.get('geometry_passing_samples_match')})
    require(len(rows) == args.expected, f'Expected {args.expected} observations, found {len(rows)}')
    result = {'schema': 1, 'kind': 'native_original_draw_validation', 'status': 'ok',
              'observations': len(rows), 'negative_controls': sum(r['negative_controls'] for r in rows),
              'pixel_mismatches': sum(r['pixel_match'] is False for r in rows),
              'sample_count_mismatches': sum(r['sample_count_match'] is False for r in rows),
              'skipped_without_observation': skipped, 'reports': rows}
    with args.out.open('x', encoding='utf-8') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(json.dumps({key: value for key, value in result.items() if key not in ('reports', 'skipped_without_observation')}))


if __name__ == '__main__':
    main()
