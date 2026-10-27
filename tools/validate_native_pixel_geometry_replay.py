"""Validate private original-PS replay evidence without GPA or RenderDoc."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def common(report, vendor):
    require(report['schema'] == 1 and report['backend'] == 'native-pixel-geometry-replay/1', 'schema/backend')
    require(report['scope'] == 'private_original_ps_geometry_range', 'private scope')
    require(report['original_fragment_eligibility'] is None, 'unknown original eligibility')
    for key in ('original_resources_written', 'original_gpu_schedule', 'pre_raster_side_effects_replayed'):
        require(report[key] is False, key)
    for key in ('original_pixel_shader_preserved', 'original_raster_and_om_state_preserved'):
        require(report[key] is True, key)
    require(report['extra_uav_slots'] == 0 and report['debug_validation'] == 'no_errors', 'isolation/validation')
    require(report['adapter']['vendor'] == (vendor or 5140), 'adapter vendor')
    require(report['adapter']['warp'] is (vendor == 0), 'explicit WARP')
    require(re.fullmatch('[0-9a-f]{64}', report['frame_sha256']) is not None, 'frame hash')
    first, count = report['first_primitive'], report['primitive_count']
    require(0 <= first <= report['geometry']['primitives'] and
            0 <= count <= report['geometry']['primitives'] - first, 'selected range')
    completed = 0
    for batch in report['batches']:
        require(batch['first_primitive'] == first + completed, 'batch start')
        require(batch['primitive_count'] > 0 and batch['passing_samples'] >= 0, 'batch count')
        require(batch['count_domain'] == 'entire_bound_output', 'query scope')
        completed += batch['primitive_count']
    require(completed == count, 'completed range')
    if count:
        bridge = report['bridge']
        require(bridge['version'] == 4 and bridge['scope'] == report['scope'], 'bridge version/scope')
        require(bridge['primitive_ordinal'] == first and bridge['primitive_count'] == count, 'bridge range')
        require(bridge['pixel_shader_sha256'] == report['pixel_shader_sha256'], 'original PS identity')
        require(bridge['original_pixel_shader_preserved'] is True and bridge['input_register'] is None,
                'complete original PS signature')
        require(report['bridge_payload_bytes'] == 2 * count * bridge['vertices_per_primitive'] * bridge['stride'],
                'bridge payload')
    require(sum(report[key] for key in ('bridge_payload_bytes', 'private_output_payload_bytes',
                                        'source_geometry_bytes')) <= 256 * 1024 * 1024, 'default payload limit')


MODES = {
    'checkpoints': ['ordinary', 'depth', 'depth_reject', 'stencil', 'stencil_reject', 'discard',
                    'early_discard', 'blend', 'color_mask', 'sample_mask', 'scissor', 'inputs', 'partial_position'],
    'msaa': [''], 'bindings': ['append', 'counter', 'classes', 'predicate_pass', 'predicate_skip'],
    'streamOutput': ['0', '1'], 'selections': ['null_ps', 'disabled', 'empty', 'subset'], 'recovery': [''],
}
FRAME_HASHES = {
    'gf2': 'fc7b7f7de20a578da2977b2ff91d96f5cf80887b393bc85859be8cab1af91c8e',
    'bf1': '26044d33cb6c3087c2204b7d57664203741a679f988430b7d05b24af582e0620',
    'hd': '97915a80c7617e9a67068a5d85b87f1b2a7883faa9831428084ebd7270f37139',
}


def owned(report, function, vendor, mode):
    common(report, vendor)
    require(report['status'] == 'ok', 'owned status')
    empty = mode in ('predicate_skip', 'disabled', 'empty')
    expected = [1, 1] if function == 'checkpoints' else [] if empty else [2] if function == 'msaa' else [1]
    if function == 'checkpoints':
        if mode in ('depth_reject', 'stencil_reject', 'sample_mask', 'scissor'):
            expected = [0, 0]
        if mode == 'discard':
            expected = [0, 1]
        require([r['completed'] for r in report['reference_prefixes']] == [1, 2], 'independent prefixes')
        prefixes = [r['passing_samples'] for r in report['reference_prefixes']]
        observed = [b['passing_samples'] for b in report['batches']]
        require(prefixes == [observed[0], sum(observed)], 'independent original query prefixes')
        if mode == 'early_discard':
            # Early-depth occlusion observations differ on local adapters.
            # Compare the original unmodified draw on the same device.
            expected = [prefixes[0], prefixes[1] - prefixes[0]]
    require([b['passing_samples'] for b in report['batches']] == expected, 'owned query oracle')
    if function == 'bindings' and mode in ('append', 'counter'):
        require(report['counter_before'] == 0 and report['counter_after_private'] == 1, 'hidden UAV counter')
    if function == 'bindings' and mode == 'classes':
        require(report['original_class_count'] == 1, 'original PS class')
    if function == 'recovery':
        require(report['recovered_failures'] == 9, 'recovery coverage')
    if mode == 'null_ps':
        require(report['pixel_shader_sha256'] is None and report['pixel_shader_resource'] == 0, 'null PS')
    if mode == 'subset':
        require(report['first_primitive'] == 1, 'nonzero range')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('owned', type=Path)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--real-prefix', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    expected_names = set()
    corruptions = 0
    for function, modes in MODES.items():
        for vendor in (0, 4318, 32902):
            for mode in modes:
                name = f'{function}-{vendor}' + (f'-{mode}' if mode else '')
                expected_names.add(name + '.json')
                report = json.loads((args.owned / (name + '.json')).read_text())
                owned(report, function, vendor, mode)
                for key, value in [('original_fragment_eligibility', True), ('original_resources_written', True),
                                   ('extra_uav_slots', 1), ('debug_validation', 'not_requested'),
                                   ('primitive_count', report['primitive_count'] + 1)]:
                    broken = copy.deepcopy(report)
                    broken[key] = value
                    try:
                        owned(broken, function, vendor, mode)
                    except ValueError:
                        corruptions += 1
                    else:
                        raise ValueError(f'Corruption was accepted: {name}/{key}')
    require({p.name for p in args.owned.glob('*.json')} == expected_names, 'exact owned case matrix')
    log = args.log.read_text()
    require('Totals: 80 passed, 0 failed, 0 skipped' in log and 'FAIL!' not in log, 'Qt run')
    real = []
    if args.real_prefix:
        for game, event, count in [('gf2', 113, 2), ('bf1', 2010, 5320), ('hd', 200, 4656)]:
            for vendor in (0, 4318, 32902):
                directory = Path(f'{args.real_prefix}-{game}-{vendor}')
                report = json.loads((directory / 'report.json').read_text())
                common(report, vendor)
                require(report['event'] == event and report['primitive_count'] == count, 'real event/range')
                require(report['frame_sha256'] == FRAME_HASHES[game], 'pinned real capture')
                p = report['probe']
                require(p['status'] == 'ok' and p['differing_bytes'] == 0 and p['original_storage_restored'] is True,
                        'real comparison')
                data = {}
                for key in ('before', 'private', 'reference', 'restored'):
                    data[key] = (directory / (key + '.bin')).read_bytes()
                    require(digest(data[key]) == p[key + '_sha256'], 'raw storage hash')
                require(data['before'] == data['restored'] and data['private'] == data['reference'], 'raw storage equality')
                require(len(data['before']) == len(data['private']), 'unchanged target extent')
                require(p['reference_passing_samples'] == sum(b['passing_samples'] for b in report['batches']),
                        'original draw query equality')
                require(p['query_matches'] is True, 'reported query equality')
                real.append({'case': directory.name, 'bytes': len(data['private']),
                             'changed_bytes': sum(a != b for a, b in zip(data['before'], data['private'])),
                             'passing_samples': p['reference_passing_samples'], 'elapsed_ms': p['elapsed_ms']})
    result = {'schema': 1, 'scope': 'private_original_ps_geometry_range', 'owned_cases': len(expected_names),
              'corruption_controls': corruptions, 'real_cases': real, 'full_pixel_history': False,
              'full_renderdoc_removal': False, 'status': 'ok'}
    with args.out.open('x') as output:
        json.dump(result, output, indent=2)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
