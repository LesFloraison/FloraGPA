"""Validate live binding snapshots used by the native merger GPU matrices."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
from validate_native_pixel_merger import cases, msaa_cases, coordinate_cases, depth_coordinate_cases, require
from validate_native_depth_only_merger import cases as depth_only_cases


def common(s):
    require((s['schema'], s['kind'], s['backend'], s['resource_observation'], s['storage_layout']) ==
            (1, 'native_pixel_merger_snapshot', 'native-pixel-merger-snapshot/1',
             'current_native_bindings', 'sample_major_native_texels'), 'Wrong binding snapshot identity')
    require(s['original_fragment_eligibility'] is None and s['content_defined'] is None and
            s['raster_inputs_supplied'] is False, 'Snapshot invented fragment or raster inputs')
    require(type(s['readback_bytes']) is int and 0 < s['readback_bytes'] <= s['readback_limit'],
            'Invalid snapshot readback accounting')
    require(1 <= len(s['targets']) <= 2, 'Missing or extra merger targets')
    for t in s['targets']:
        require(type(t['resource']) is int and t['resource'] > 0 and type(t['view']) is int and t['view'] > 0,
                'Missing GPA target identity')
        sub = t['subresource']
        require(sub['source_view'] == ('dsv' if t['depth'] else 'rtv') and sub['width'] > 0 and sub['height'] > 0,
                'Wrong bound view shape')
        require(sub['layer'] == sub['first_layer'] + sub['relative_layer'] and
                0 <= sub['relative_layer'] < sub['layer_count'], 'Wrong view-relative layer')
        require(t['invalid_byte_offsets'] == ([5, 6, 7] if sub['format'] == 20 else []),
                'Wrong storage byte validity')
        require(type(t['exact_storage_bits']) is bool and t['transfer'] in ('selected_sample', 'native_storage_copy'),
                'Missing transfer precision')


def merged(document):
    r = document['result']
    s = r['binding_snapshot']
    common(s)
    require(r['depth_stencil_descriptor'] == r['reference_depth_stencil_descriptor'],
            'Observed state differs from independent D3D11 creation')
    require(len(s['targets']) == int(r['color_format'] != 0) + int(r['depth_format'] != 0),
            'Snapshot target count differs from merger')
    require(len({t['depth'] for t in s['targets']}) == len(s['targets']), 'Duplicate output target role')
    for t in s['targets']:
        sub = t['subresource']
        require(sub['format'] == r['depth_format' if t['depth'] else 'color_format'] and
                [sub['width'], sub['height']] == r['private_target_extent'] and
                sub['mip'] == sub['layer'] == sub['relative_layer'] == 0,
                'Snapshot selection differs from fixture merger')
        if t['depth']:
            require(sub['dsv_flags'] == r['depth_view_flags'], 'Wrong observed DSV flags')
        require(t['exact_storage_bits'] == (r['sample_count'] == 1 or sub['format'] != 87) and
                t['transfer'] == ('selected_sample' if r['sample_count'] > 1 else 'native_storage_copy'),
                'Unsupported exactness claim for fixture transfer')
    require(s['readback_bytes'] >= len(r['color_before']) + len(r['depth_before']), 'Missing target readback')


def selected(r, vendor, mode):
    common(r)
    require(r['vendor_request'] == vendor and r['mode'] == mode and r['retry_verified'] is True and
            r['cancellation_checks'] > 6, 'Wrong selection/cancellation fixture')
    t = r['targets'][0]
    require(len(r['targets']) == 1 and not t['depth'] and t['resource'] == 1000 and t['view'] == 1002,
            'Wrong selected output identity')
    sub = t['subresource']
    require(t['exact_storage_bits'] is True, 'Lost exact integer storage')
    msaa, volume, one = mode == 'msaa', mode == 'volume', mode == 'array1d'
    require(sub['format'] == 3 and sub['mip'] == (0 if msaa else 1) and sub['relative_layer'] == 1 and
            sub['layer'] == (1 if volume else 2) and sub['layer_count'] == 2 and
            sub['width'] == (8 if msaa else 4) and sub['height'] == (1 if one else 4 if msaa else 2),
            'Wrong array/mip/slice selection')
    tag = 3 + (0 if one else 16) + (64 if volume else 512) + 4096
    expected = list(struct.pack('<4I', 1, 2, 3, 4)) * 4 if msaa else list(
        struct.pack('<4I', tag, tag ^ 0x80000000, tag + 5, 0xdeadbeef))
    require(r['color'] == r['expected_color'] == expected, 'Independent selected texel differs')
    require(r['readback_bytes'] == {'array1d': 576, 'array2d': 1920, 'volume': 2304, 'msaa': 6144}[mode],
            'Wrong complete readback budget')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    matrices = [('', cases), ('msaa', msaa_cases), ('coordinates', coordinate_cases),
                ('depthCoordinates', depth_coordinate_cases), ('depthOnly', depth_only_cases)]
    tasks = []
    for folder, factory in matrices:
        expected = {name for name, _ in factory()}
        require({f.stem for f in (args.directory / folder).glob('*.json')} == expected,
                'Incomplete merger snapshot matrix: ' + folder)
        tasks += [(args.directory / folder / (name + '.json'), merged, ()) for name in sorted(expected)]
    expected = {f'{vendor}-{mode}': (vendor, mode) for vendor in (0, 4318, 32902)
                for mode in ('array1d', 'array2d', 'volume', 'msaa')}
    require({f.stem for f in (args.directory / 'snapshotSelection').glob('*.json')} == set(expected),
            'Incomplete snapshot selection matrix')
    tasks += [(args.directory / 'snapshotSelection' / (name + '.json'), selected, selection)
              for name, selection in sorted(expected.items())]
    report = dict(schema=1, scope='native_merger_snapshot_validation', completed=False,
                  cases=0, negative_controls=0, files={})
    for path, validate, selection in tasks:
        raw = path.read_bytes()
        r = json.loads(raw)
        validate(r, *selection)
        report['cases'] += 1
        report['files'][str(path.relative_to(args.directory))] = hashlib.sha256(raw).hexdigest()
        for key, value in [('backend', 'legacy'), ('original_fragment_eligibility', True), ('content_defined', True),
                           ('raster_inputs_supplied', True), ('readback_bytes', 0), ('readback_limit', 0), ('targets', [])]:
            bad = copy.deepcopy(r)
            snapshot = bad['result']['binding_snapshot'] if 'result' in bad else bad
            snapshot[key] = value
            try:
                validate(bad, *selection)
            except ValueError:
                report['negative_controls'] += 1
            else:
                raise ValueError('Accepted invalid snapshot: ' + key)
        bad = copy.deepcopy(r)
        snapshot = bad['result']['binding_snapshot'] if 'result' in bad else bad
        snapshot['targets'][0]['exact_storage_bits'] = not snapshot['targets'][0]['exact_storage_bits']
        try:
            validate(bad, *selection)
        except ValueError:
            report['negative_controls'] += 1
        else:
            raise ValueError('Accepted incorrect transfer precision')
    report['completed'] = True
    with args.out.open('x', encoding='utf-8') as output:
        json.dump(report, output, indent=2)
        output.write('\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'files'}))


if __name__ == '__main__':
    main()
