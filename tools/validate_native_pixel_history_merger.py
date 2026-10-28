"""Validate observed state, native PS output and private GPU checkpoint joins."""
import argparse
import copy
import hashlib
import json
import struct
from pathlib import Path

from validate_native_pixel_history_shader import validate as shader, fixture, VENDORS, MODES, UAVS
from validate_native_merger_snapshots import common as snapshot
from validate_native_pixel_merger import require
from validate_native_pixel_geometry_replay import FRAME_HASHES


def validate(r, vendor):
    shader(r, vendor)
    stats = r['merger_evaluation']
    require(stats['requested'] is True and stats['original_fragment_eligibility'] is None and
            stats['resource_boundary'] == 'before_private_primitive', 'Merger execution scope')
    count = reads = 0
    selection = r['selection']
    for row in r['records']:
        joins = row['private_output_mergers']
        require(len(joins) == len(row['private_shader_outputs']), 'Missing shader/merger join')
        for index, joined in enumerate(joins):
            count += 1
            require(joined['scope'] == 'one_private_shader_lane_on_selected_storage_sample' and
                    joined['shader_output_index'] == index and
                    joined['original_fragment_eligibility'] is None, 'Wrong join identity')
            out = row['private_shader_outputs'][index]
            m, s = joined['merger'], joined['binding_snapshot']
            snapshot(s)
            if index == 0:
                reads += s['readback_bytes']
            else:
                require(s == joins[0]['binding_snapshot'], 'Primitive binding snapshot changed')
            require(m['schema'] == 1 and m['backend'] == 'native-pixel-merger/4' and
                    m['kind'] == 'native_pixel_merger' and m['original_fragment_eligibility'] is None and
                    m['original_early_depth_reconstructed'] is False, 'Unproved merger execution')
            require(m['adapter'] == out['adapter'] and m['lane'] == out['selected_lane'] and
                    m['debug_validation'] == 'no_errors', 'Merger adapter, lane or validation')
            source = json.dumps(out, sort_keys=True, ensure_ascii=False, separators=(',', ':')).encode()
            require(m['source_sha256'] == hashlib.sha256(source).hexdigest(), 'Shader output digest')
            raster = joined['raster_inputs']
            require(raster == out['input_provenance']['selected_raster'] and
                    m['front_face'] == raster['front_face'] and
                    m['coverage_mask'] == raster['coverage_mask'], 'Measured raster join')
            if 'sample_depth_observation' in row:
                require(joined.get('raster_depth_source') == 'sample_position_observation', 'Lost depth source')
            if joined.get('raster_depth_source') == 'sample_position_observation':
                depth = row['sample_depth_observation']
                observed = depth['observation']
                target = observed['raster_state']['target']
                require(depth['schema'] == 1 and depth['kind'] == 'native_pixel_sample_depth' and
                        depth['backend'] == 'native-pixel-sample-depth/1' and
                        depth['scope'] == 'isolated_primitive_sample_position' and
                        depth['position_interpolation'] == 'linear_noperspective_sample' and
                        depth['original_fragment_eligibility'] is None, 'Sample-depth observation identity')
                require(len(depth['position_bits']) == 4 and depth['position_bits'][2] == depth['depth_bits'],
                        'Sample-position raw bits')
                require(depth['geometry_sha256'] == out['input_provenance']['geometry_sha256'] and
                        depth['primitive_ordinal'] == row['primitive_ordinal'] and
                        depth['sample'] == selection['sample'] and depth['front_face'] == raster['front_face'] and
                        depth['coverage_mask'] & (1 << selection['sample']) and
                        m['raster_depth_bits'] == depth['depth_bits'], 'Sample-depth input join')
                require(observed['records'] == 1 and observed['sample_frequency'] is True and
                        target['depth'] is True and target['format'] == m['depth_format'] and
                        observed['adapter_luid'] == m['adapter']['luid'] and
                        observed['adapter_vendor'] == m['adapter']['vendor'] and
                        observed['adapter_device'] == m['adapter']['device'] and
                        observed['layer'] == selection['subresource']['relative_layer'] and
                        observed['geometry']['primitive_ordinal'] == row['primitive_ordinal'] and
                        observed['geometry']['original_primitive_id'] == row['original_primitive_id'] and
                        [observed['x'], observed['y'], observed['sample']] ==
                        [selection['x'], selection['y'], selection['sample']], 'Sample-depth hardware context')
            else:
                require(m['raster_depth_bits'] == raster['depth_bits'], 'Legacy or unused depth join')
            sample = selection['sample']
            require(joined['selected_sample'] == sample and 0 <= sample < m['sample_count'] and
                    m['sample_mask'] == joined['application_sample_mask'] & (1 << sample) and
                    m['effective_sample_mask'] == m['sample_mask'] & m['coverage_mask'], 'Selected sample mask')
            require(m['private_pixel_location'] == [selection['x'], selection['y']] and
                    m['private_target_extent'] == [selection['subresource']['width'], selection['subresource']['height']],
                    'Merger location or extent')
            targets = [t for t in s['targets'] if t['resource'] == selection['resource']]
            require(len(targets) == 1, 'Selected target identity')
            target = targets[0]
            require(target['view'] == selection['view'], 'Selected view identity')
            for key in ('mip', 'layer', 'slice', 'array_layer', 'relative_layer', 'format'):
                require(target['subresource'][key] == selection['subresource'][key], 'Selected subresource')
            prefix = 'depth' if target['depth'] else 'color'
            require(m[prefix + '_format'] == row['after']['typed_format'], 'Selected target format')
            size = len(row['after']['bytes'])
            begin = sample * size
            require(len(m[prefix + '_before']) == len(m[prefix + '_after']) == size * m['sample_count'],
                    'Selected target storage shape')
            before = m[prefix + '_before'][begin:begin + size]
            after = m[prefix + '_after'][begin:begin + size]
            require(joined['selected_after'] == after and
                    joined['selected_exact_storage_bits'] == target['exact_storage_bits'], 'Selected result precision')
            require(all(not valid or x == y for x, y, valid in zip(before, row['before']['bytes'], row['before']['valid_bytes'])),
                    'Merger input differs from before-primitive checkpoint')
            same = all(not valid or x == y for x, y, valid in zip(after, row['after']['bytes'], row['after']['valid_bytes']))
            require(joined['observed_value_matches_checkpoint'] is same and same, 'GPU checkpoint mismatch')
            exact = target['exact_storage_bits'] and row['after']['exact_storage_bits']
            require(joined['exact_storage_matches_checkpoint'] is (same if exact else None), 'Checkpoint precision')
            require(m['count_semantics'] == 'd3d11_occlusion_query' and
                    type(m['passing_samples']) is int and 0 <= m['passing_samples'] <= 1, 'Selected sample query')
    require(stats['invocations'] == count == r['shader_evaluation']['invocations'] and
            stats['readback_bytes'] == reads, 'Merger accounting')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('geometry', type=Path)
    p.add_argument('checkpoint', type=Path)
    p.add_argument('--real-prefix', type=Path)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    expected = {f'shaderHistory-{v}-{m}-{s}': (args.geometry, v) for v in VENDORS for m in MODES for s in (0, 3)}
    expected.update({f'shaderHistoryBoundaries-{v}-{m}': (args.geometry, v) for v in VENDORS
                     for m in ('depth_reject', 'stencil_reject', 'null_ps', 'forced')})
    expected.update({f'shaderHistoryRecovery-{v}': (args.geometry, v) for v in VENDORS})
    expected.update({f'shaderHistory-{v}-{m}': (args.checkpoint, v) for v in VENDORS for m in UAVS})
    expected.update({f'mergerHistory-{v}-{s}-{m}': (args.geometry, v) for v in VENDORS for s in (1, 4)
                     for m in ('blend', 'dual', 'alpha', 'write-mask', 'unchanged')})
    tasks = []
    for name, (directory, vendor) in sorted(expected.items()):
        path = directory / (name + '.json')
        raw = path.read_bytes()
        report = json.loads(raw)
        if name.startswith('mergerHistory-'):
            require(len(report['records']) == len(report['reference_prefix_bits']) == 2, 'Independent prefixes')
            mode = report['mode']
            source, previous = [.5, .25, .75, .5], [.125, .5, .25, .25]
            for row, reference in zip(report['records'], report['reference_prefix_bits']):
                bits = list(struct.unpack('<4I', bytes(row['after']['bytes'])))
                require(bits == reference, 'Original independent prefix differs')
                if mode in ('blend', 'dual'):
                    weights = [.25, .75, .5] if mode == 'dual' else [.5]*3
                    expected_value = [source[i]*weights[i] + previous[i]*(1-weights[i]) for i in range(3)] + [.5]
                elif mode == 'write-mask':
                    expected_value = [.5, .5, .75, .25]
                elif mode == 'unchanged':
                    expected_value = previous
                    require(row['observed_value_changed'] is False and
                            row['private_output_mergers'][0]['merger']['passing_samples'] == 1,
                            'Lost unchanged write execution')
                else:
                    require(mode == 'alpha', 'Unknown integration fixture')
                    expected_value = source if row['private_output_mergers'][0]['merger']['passing_samples'] else previous
                require(bits == list(struct.unpack('<4I', struct.pack('<4f', *expected_value))),
                        'Independent merger arithmetic differs')
                previous = expected_value
        else:
            fixture(name, report, vendor)
        require(report['status'] == 'ok', 'Owned probe failure')
        tasks.append((path, raw, report, vendor))
    if args.real_prefix:
        for game in ('gf2', 'bf1', 'hd'):
            for vendor in VENDORS:
                directory = Path(str(args.real_prefix) + f'-{game}-{vendor}')
                path = directory / 'report.json'
                raw = path.read_bytes()
                report = json.loads(raw)
                require(report['frame_sha256'] == FRAME_HASHES[game] and report['probe']['status'] == 'ok',
                        'Real probe identity or result')
                require((directory / 'reference.bin').read_bytes() == (directory / 'private.bin').read_bytes() ==
                        bytes(report['after']['bytes']), 'Real GPU reference')
                tasks.append((path, raw, report, vendor))
    controls = 0
    files = {}
    for path, raw, report, vendor in tasks:
        validate(report, vendor)
        files[str(path)] = hashlib.sha256(raw).hexdigest()
        mutations = [lambda r: r['merger_evaluation'].update(invocations=-1),
                     lambda r: r['merger_evaluation'].update(readback_bytes=-1),
                     lambda r: r['merger_evaluation'].update(original_fragment_eligibility=True)]
        if report['merger_evaluation']['invocations']:
            for key, value in [('scope', 'original_fragment'), ('shader_output_index', 99999),
                               ('selected_sample', 32), ('selected_after', []),
                               ('observed_value_matches_checkpoint', False)]:
                mutations.append(lambda r, k=key, v=value: r['records'][0]['private_output_mergers'][0].update({k: v}))
            mutations += [lambda r: r['records'][0]['private_output_mergers'][0]['merger'].update(source_sha256='0'*64),
                          lambda r: r['records'][0]['private_output_mergers'][0]['binding_snapshot'].update(content_defined=True),
                          lambda r: r['records'][0]['private_output_mergers'][0]['raster_inputs'].update(depth_bits=-1)]
        for mutate in mutations:
            broken = copy.deepcopy(report)
            mutate(broken)
            try:
                validate(broken, vendor)
            except ValueError:
                controls += 1
            else:
                raise ValueError('Accepted corrupted merger join: ' + str(path))
    result = dict(schema=1, completed=True, owned_cases=len(expected), real_cases=len(tasks)-len(expected),
                  negative_controls=controls, complete_pixel_history=False, files=files)
    with args.out.open('x', encoding='utf-8') as f:
        json.dump(result, f, indent=2)
        f.write('\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'files'}))


if __name__ == '__main__':
    main()
