"""Validate private primitive-storage joins without GPA or RenderDoc."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import math
from validate_native_pixel_geometry_replay import common, FRAME_HASHES, require


def storage(value):
    require(value['defined'] is None and type(value['exact_storage_bits']) is bool, 'storage provenance')
    require(len(value['bytes']) == len(value['valid_bytes']) > 0, 'storage shape')
    require(all(type(b) is int and 0 <= b <= 255 for b in value['bytes']), 'storage bytes')
    require(all(type(b) is bool for b in value['valid_bytes']), 'byte validity')


def same(a, b):
    storage(a)
    storage(b)
    require(a['typed_format'] == b['typed_format'] and a['valid_bytes'] == b['valid_bytes'], 'stable interpretation')
    return all(not valid or x == y for x, y, valid in zip(a['bytes'], b['bytes'], a['valid_bytes']))


def validate(r, vendor):
    require(r['schema'] == 1 and r['backend'] == 'native-pixel-draw-history/5', 'schema/backend')
    require(r['kind'] == 'private_pixel_draw_history', 'private history kind')
    require(r['order'] == 'captured_geometry_storage' and r['sample_selection_domain'] == 'target_storage', 'order/sample scope')
    require(r['original_fragment_eligibility'] is None, 'original eligibility')
    for key in ('fragment_history_available', 'original_gpu_schedule', 'pre_raster_side_effects_replayed'):
        require(r[key] is False, key)
    replay = r['geometry_replay']
    common(replay, vendor)
    require(r['event'] == replay['event'] and r['frame_sha256'] == replay['frame_sha256'] and
            r['adapter'] == replay['adapter'], 'joined execution identity')
    original_reads = 0
    if 'original_draw_observation' in r:
        from validate_native_original_draw import validate as original_draw
        original_draw(r)
        original_reads = 2
    count = r['return_count_observation']
    require(type(count['requested']) is bool and count['original_fragment_eligibility'] is None,
            'Return count provenance')
    if not count['requested']:
        require(count['status'] == 'not_requested' and count['count'] is None and 'observation' not in count,
                'Unrequested count')
    elif count['status'] == 'not_applicable':
        expected_reason = ('selected_depth_target' if r['selection']['subresource']['slot'] is None
                           else 'no_pixel_shader' if r['original_draw_observation']['shaders'][4]['resource'] == 0
                           else None)
        require(expected_reason is not None and count['reason'] == expected_reason and count['count'] is None
                and 'observation' not in count, 'Inapplicable count')
    else:
        observed = count['observation']
        for key in ('event', 'frame_sha256', 'adapter', 'shaders'):
            require(observed[key] == r['original_draw_observation'][key], 'Return count context: ' + key)
        selection = r['selection']
        depth = selection['subresource']['slot'] is None
        transport = observed['depth_transport' if depth else 'count_transport']
        if depth:
            from validate_depth_return_count import verify as verify_depth
            verify_depth(observed, vendor)
        else:
            require(transport['source_slot'] == selection['subresource']['slot'], 'Count source slot')
        require([transport[k] for k in ('x', 'y', 'sample', 'layer', 'source_mip')]
                == [selection['x'], selection['y'], selection['sample'],
                    selection['subresource']['relative_layer'], selection['mip']], 'Return count selection')
        require(transport['source_format'] == selection['subresource']['format']
                and transport['source_first_layer'] == selection['subresource']['first_layer'], 'Return count source view')
        require(count['count'] == observed['return_count']['count']
                and observed['original_fragment_eligibility'] is None
                and (depth or observed['backend'] == 'native-pixel-return-count/3'), 'Return count contract')
        decoded = observed['return_count']
        if decoded['valid'] is True:
            from validate_pixel_return_count import verify
            require(count['status'] == 'observed', 'Valid count status')
            component = observed['completion_shader']['component_type']
            require(component in (1, 2, 3), 'Return count component type')
            if not depth:
                verify(observed, vendor, component_type=component)
        else:
            bits = decoded['raw_bits']
            require(len(bits) == 4 and all(type(x) is int and 0 <= x <= 0xffffffff for x in bits), 'Unknown raw count')
            value = struct.unpack('<f', struct.pack('<I', bits[0]))[0]
            integral = math.isfinite(value) and value >= 0 and math.floor(value) == value
            reason = ('rgb_disagreement' if bits[:3] != [bits[0]] * 3 else
                      'invalid_accumulation' if not integral else 'float32_integer_limit' if value >= 2**24 else None)
            require(reason is not None and decoded['reason'] == reason and decoded['valid'] is False
                    and count['status'] == 'unknown' and count['count'] is None, 'Unknown count cannot become zero')
    identity_reads = 0
    if r.get('primitive_identity_observation', {}).get('requested') is True:
        from validate_native_pixel_identity import validate_history_identity
        identity_reads = validate_history_identity(r)
    presence_reads = 0
    if r.get('primitive_presence_observation', {}).get('requested') is True:
        from validate_native_pixel_presence import history_presence
        presence_reads = history_presence(r)
    require(r['readback_count'] == len(replay['batches']) + 3 + original_reads + identity_reads + presence_reads, 'checkpoint read count')
    require(r['observed_storage_restored'] is True and same(r['before'], r['restored']), 'restored storage')
    require(r['original_storage_restored'] is (True if r['before']['exact_storage_bits'] else None), 'restoration precision')
    require(r['unattributed_changes'] == [], 'accepted cases have no unexplained storage transitions')
    expected, referenced = {}, set()
    from validate_pixel_geometry_instances import partition
    instance_mapping = partition(replay['geometry'])
    for index, candidate in enumerate(r['candidate_passes']):
        require(candidate['geometry_metadata'] == replay['geometry'], 'same captured geometry')
        require(candidate['original_fragment_eligibility'] is None and candidate['order'] == 'unspecified', 'candidate scope')
        require(candidate['x'] == r['selection']['x'] and candidate['y'] == r['selection']['y'] and
                candidate['layer'] == r['selection']['subresource']['relative_layer'], 'candidate location')
        for row in candidate['observations']:
            if 'geometry_instances' in replay['geometry']:
                require(row['geometry_instance'] == (instance_mapping(row['primitive_ordinal']) if instance_mapping else None),
                        'Candidate geometry instance mapping')
            expected[(index, row['observation_index'])] = row
    ordinals = [row['primitive_ordinal'] for row in r['records']]
    require(ordinals == sorted({row['primitive_ordinal'] for row in expected.values()}), 'complete ordered candidate union')
    previous = r['before']
    for row in r['records']:
        require(row['event'] == r['event'] and row['record_kind'] == 'private_primitive_storage', 'record provenance')
        for key in ('instance', 'original_fragment_eligibility', 'write_executed', 'shader_output', 'failure_reasons'):
            require(row[key] is None, key)
        if 'geometry_instances' in replay['geometry']:
            require(row['geometry_instance'] == (instance_mapping(row['primitive_ordinal']) if instance_mapping else None),
                    'History geometry instance mapping')
        require(any(b['first_primitive'] == row['primitive_ordinal'] and b['primitive_count'] == 1
                    for b in replay['batches']), 'isolated candidate checkpoint')
        require(same(previous, row['before']), 'prefix storage continuity')
        changed = not same(row['before'], row['after'])
        require(row['observed_value_changed'] is changed, 'observed transition')
        exact = row['before']['exact_storage_bits'] and row['after']['exact_storage_bits']
        require(row['storage_changed'] is (changed if exact else None), 'storage transition precision')
        require(len(row['observations']) > 0, 'candidate observations')
        for reference in row['observations']:
            key = (reference['pass'], reference['observation_index'])
            require(key in expected and key not in referenced, 'unique observation reference')
            source = expected[key]
            require(source['primitive_ordinal'] == row['primitive_ordinal'] and
                    source['original_primitive_id'] == row['original_primitive_id'] and
                    source['selected_lane'] == reference['selected_lane'], 'observed identity join')
            require(reference['raster_sample'] == r['candidate_passes'][key[0]]['sample'], 'raster sample join')
            referenced.add(key)
        previous = row['after']
    require(referenced == set(expected) and same(previous, r['after']), 'complete storage/observation join')


MODES = ['ordinary', 'blend', 'depth', 'stencil', 'discard', 'early_discard', 'mask', 'scissor', 'cull',
         'clip', 'unchanged', 'depth_resource']


def oracle(r, function, tag):
    raw = r['after']['bytes']
    if function == 'drawHistory':
        empty = tag in ('scissor', 'cull', 'clip')
        require(len(r['records']) == (0 if empty else 2), 'owned candidate count')
        require(len(r['original_prefixes']) == len(r['records']), 'independent original prefixes')
        for index, row in enumerate(r['records']):
            require(r['original_prefixes'][index] == {'completed_primitives': index + 1, 'bytes': row['after']['bytes']},
                    'exact original prefix bytes')
        if empty or tag in ('depth', 'stencil', 'mask'):
            require(same(r['before'], r['after']), 'rejected output')
        elif tag in ('discard', 'early_discard'):
            # Interpolated position components are observed from independent
            # original draws above, not replaced with a CPU pixel-center guess.
            require(raw[2:] == [64, 255], 'constant discard output components')
        else:
            expected = {'ordinary': [255, 0, 0, 255], 'unchanged': [255, 0, 0, 255],
                        'blend': [128, 64, 192, 255], 'depth_resource': [0, 0, 32, 83]}[tag]
            require(raw == expected, 'independent owned storage oracle')
    elif function == 'drawHistoryMsaa':
        sample = int(tag)
        require(r['selection']['sample'] == sample and len(r['records']) == 1, 'MSAA selection')
        if sample in (1, 3):
            require(same(r['before'], r['after']), 'masked MSAA sample')
        else:
            value = struct.unpack('<4f', bytes(raw))
            require(value[0] == sample * .25 and value[3] == .5, 'sample-frequency output')
    elif function == 'drawHistoryLayers':
        volume, layer = map(int, tag.split('-'))
        s = r['selection']
        require(s['mip'] == 1 and s['layer'] == (0 if volume else layer + 1) and
                s['slice'] == (layer if volume else 0), 'absolute mip/layer/slice')
        require(s['subresource']['relative_layer'] == layer, 'relative raster layer')
        require([row['primitive_ordinal'] for row in r['records']] == [layer] and
                r['records'][0]['original_primitive_id'] is None, 'layered GS identity')
        require(bytes(raw) == struct.pack('<4f', 1.5, 1.5, .25, 1), 'layered storage oracle')
    elif function == 'drawHistoryForced':
        require(len(r['candidate_passes']) == 4 and len(r['records']) == 1 and
                len(r['records'][0]['observations']) == 4, 'forced raster sample union')
        require(bytes(raw) == struct.pack('<4f', .5, .25, .75, .5), 'forced-sample output')
    else:
        require(len(r['records']) == 2, 'successful recovery')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('owned', type=Path)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--real-prefix', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    cases = {'drawHistory': MODES, 'drawHistoryMsaa': [str(i) for i in range(4)],
             'drawHistoryLayers': [f'{volume}-{layer}' for volume in range(2) for layer in range(2)],
             'drawHistoryForced': [''], 'drawHistoryRecovery': ['']}
    count = rejected = 0
    for function, tags in cases.items():
        for vendor in (0, 4318, 32902):
            for tag in tags:
                name = f'{function}-{vendor}' + (f'-{tag}' if tag else '')
                r = json.loads((args.owned / (name + '.json')).read_text())
                validate(r, vendor)
                oracle(r, function, tag)
                count += 1
                corruptions = []
                for key, value in [('original_fragment_eligibility', True), ('fragment_history_available', True),
                                   ('frame_sha256', '0' * 64), ('readback_count', 0)]:
                    bad = copy.deepcopy(r)
                    bad[key] = value
                    corruptions.append(bad)
                bad = copy.deepcopy(r)
                bad['restored']['bytes'][0] ^= 1
                corruptions.append(bad)
                if r['records']:
                    for key, value in [('write_executed', True), ('primitive_ordinal', 999999),
                                       ('original_primitive_id', 999999)]:
                        bad = copy.deepcopy(r)
                        bad['records'][0][key] = value
                        corruptions.append(bad)
                for bad in corruptions:
                    try:
                        validate(bad, vendor)
                    except ValueError:
                        rejected += 1
                    else:
                        raise ValueError('Corrupted private history was accepted: ' + name)
    log = args.log.read_text()
    require('Totals: 146 passed, 0 failed, 0 skipped' in log and 'FAIL!' not in log, 'complete Qt matrix')
    real = []
    if args.real_prefix:
        for game, event in [('gf2', 113), ('bf1', 2010), ('hd', 200)]:
            for vendor in (0, 4318, 32902):
                directory = Path(f'{args.real_prefix}-{game}-{vendor}')
                r = json.loads((directory / 'report.json').read_text())
                validate(r, vendor)
                require(r['frame_sha256'] == FRAME_HASHES[game] and r['event'] == event, 'pinned real selection')
                actual, reference = (directory / 'private.bin').read_bytes(), (directory / 'reference.bin').read_bytes()
                require(actual == reference and actual == bytes(r['after']['bytes']) and r['probe']['status'] == 'ok', 'real output equality')
                for key, data in [('private', actual), ('reference', reference)]:
                    require(hashlib.sha256(data).hexdigest() == r['probe'][key + '_sha256'], 'raw reference hash')
                real.append({'case': directory.name, 'records': len(r['records']),
                             'changed_records': sum(row['storage_changed'] is True for row in r['records']),
                             'readback_count': r['readback_count'], 'readback_bytes': r['readback_bytes'],
                             'elapsed_ms': r['probe']['elapsed_ms']})
    result = {'schema': 1, 'scope': 'private_primitive_storage', 'owned_cases': count,
              'corruption_controls': rejected, 'real_cases': real, 'full_pixel_history': False,
              'full_renderdoc_removal': False, 'status': 'ok'}
    with args.out.open('x') as output:
        json.dump(result, output, indent=2)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
