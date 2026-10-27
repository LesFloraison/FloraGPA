"""Validate depth-only merger reports against original GPU draws and numeric expectations."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct

import validate_native_pixel_merger as common

require = common.require
MODES = ('raster', 'shader-depth', 'discard', 'sample-mask', 'coverage', 'alpha',
         'no-depth-write', 'read-only-depth', 'depth-fail', 'stencil-pass',
         'stencil-fail', 'stencil-back', 'read-only-stencil', 'unchanged')


def cases():
    for vendor in (0, 4318, 32902):
        for samples in (1, 2, 4):
            for fmt in (20, 40, 45, 55):
                for mode in MODES:
                    if 'stencil' not in mode or fmt in (20, 45):
                        yield f'{vendor}-{samples}-{fmt}-{mode}', (vendor, samples, fmt, mode)


def validate(document, vendor, samples, fmt, mode):
    r, q = document['result'], document['quad_outputs']
    common.EXPECTED_BACKEND = 'native-pixel-merger/4'
    common.validate_location(r, (6, 4), (7, 5))
    require((r['schema'], r['kind'], r['scope']) ==
            (1, 'native_pixel_merger', 'private_single_sample_output_merger' if samples == 1 else
             'private_multisample_output_merger'), 'Wrong report identity')
    require(r['sample_count'] == samples and r['sample_quality'] == 0 and
            r['storage_layout'] == 'sample_major_native_texels', 'Wrong sample layout')
    require(r['original_fragment_eligibility'] is None and r['original_early_depth_reconstructed'] is False,
            'Invented original fragment execution')
    require(r['reference_storage_unchanged'] is True and r['debug_validation'] == 'no_errors',
            'Failed isolated execution')
    require(r['adapter'] == q['adapter'] and r['adapter']['vendor'] == (vendor or 5140) and
            r['adapter']['luid'] != 0, 'Wrong adapter')
    source = json.dumps(q, sort_keys=True, ensure_ascii=False, separators=(',', ':')).encode()
    require(r['source_sha256'] == hashlib.sha256(source).hexdigest(), 'Wrong source digest')
    require(q['event'] == 1000 and r['lane'] == 0, 'Wrong event/lane')
    require(r['color_format'] == r['color_slot'] == 0 and r['depth_format'] == fmt and
            r['color_before'] == r['color_after'] == r['reference_color'] == [], 'Unexpected color target')
    require(r['color_invalid_byte_offsets'] == [] and
            r['depth_invalid_byte_offsets'] == ([5, 6, 7] if fmt == 20 else []), 'Wrong storage validity')
    require(r['depth_after'] == r['reference_depth'], 'Original GPU depth/stencil differs')
    size, depth_size = {20: (8, 4), 40: (4, 4), 45: (4, 3), 55: (2, 2)}[fmt]
    for key in ('depth_before', 'depth_after'):
        require(len(r[key]) == size * samples and all(type(v) is int and 0 <= v <= 255 for v in r[key]),
                'Invalid depth/stencil bytes')
    front = mode != 'stencil-back'
    stencil = fmt in (20, 45) and mode != 'unchanged'
    face = [3 if mode == 'stencil-fail' else 1, 6 if mode == 'depth-fail' else 1,
            6 if not front else 3, 1 if mode == 'stencil-fail' else 8]
    desc = [1, 0 if mode == 'no-depth-write' else 1,
            8 if mode == 'unchanged' else 1 if mode == 'depth-fail' else 2,
            int(stencil), 65535] + (face + [1, 1, 1, 8] if front else [1, 1, 1, 8] + face)
    require(r['depth_stencil_descriptor'] == desc and r['front_face'] == front and r['stencil_ref'] == 0xa5,
            'Wrong depth/stencil state')
    flags = 1 if mode == 'read-only-depth' else 2 if mode == 'read-only-stencil' else 0
    require(r['depth_view_flags'] == flags, 'Wrong read-only flags')
    mask = 0x55555555 if mode == 'sample-mask' else 0xffffffff
    require(r['sample_mask'] == r['effective_sample_mask'] == mask and r['coverage_mask'] == 0xffffffff,
            'Wrong coverage inputs')
    require(r['raster_depth_bits'] == (0x3f000000 if mode == 'unchanged' else 0), 'Wrong raster depth')
    require(r['blend_descriptor'][0] == int(mode == 'alpha'), 'Wrong alpha-to-coverage state')
    require(q['lanes'][0]['discarded'] == (mode == 'discard'), 'Wrong shader discard')
    if mode == 'shader-depth':
        require(q['lanes'][0]['outputs'][0]['raw_bits'][0] == 0x3e800000, 'Wrong shader depth')
    if mode == 'coverage':
        require(q['lanes'][0]['outputs'][0]['raw_bits'][0] == 5, 'Wrong shader coverage')
    if mode == 'alpha':
        require(q['lanes'][0]['outputs'][0]['raw_bits'][3] == 0x3ec00000, 'Wrong shader alpha')
    require(r['count_semantics'] == 'd3d11_occlusion_query' and
            type(r['passing_samples']) is int and 0 <= r['passing_samples'] <= samples, 'Wrong query')
    count = 0
    for sample in range(samples):
        before = r['depth_before'][sample * size:(sample + 1) * size]
        after = r['depth_after'][sample * size:(sample + 1) * size]
        depth = (int.from_bytes(bytes(before[:depth_size]), 'little') / ((1 << (depth_size * 8)) - 1)
                 if fmt in (45, 55) else struct.unpack('<f', bytes(before[:4]))[0])
        require(abs(depth - .5) <= 1 / ((1 << (depth_size * 8)) - 1), 'Wrong initial depth')
        if fmt in (20, 45):
            require(before[depth_size] == 0x53, 'Wrong initial stencil')
        if fmt == 20:
            require(before[5:] == after[5:] == [0, 0, 0], 'Invalid padding was not normalized')
        covered = bool(mask & (1 << sample)) and (mode != 'coverage' or bool(5 & (1 << sample)))
        if mode == 'discard':
            covered = False
        # Alpha-to-coverage is hardware-defined. Classify the selected sample
        # only against the original draw, then verify both allowed outcomes.
        if mode == 'alpha':
            covered = after != before
        passed = covered and mode not in ('stencil-fail', 'depth-fail')
        count += int(passed)
        expected = before.copy()
        if passed and mode not in ('no-depth-write', 'read-only-depth', 'unchanged'):
            incoming = .25 if mode == 'shader-depth' else 0
            expected[:depth_size] = (list(round(incoming * ((1 << (depth_size * 8)) - 1)).to_bytes(depth_size, 'little'))
                                     if fmt in (45, 55) else list(struct.pack('<f', incoming)))
        if covered and stencil and mode != 'read-only-stencil':
            expected[depth_size] = 0xac if mode in ('stencil-back', 'depth-fail') else 0xa5
        require(after == expected, 'Independent depth/stencil result differs')
    require(r['passing_samples'] == count, 'Selected sample query differs')
    full = r['reference_entire_draw_passing_samples']
    require(type(full) is int and 0 <= full <= 35 * samples, 'Invalid original draw query')
    if mode != 'alpha':
        require(full == 35 * count, 'Original full-draw query differs')
    if mode == 'unchanged':
        require(r['depth_before'] == r['depth_after'] and count == samples, 'Lost same-value write coverage')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    matrix = list(cases())
    require({p.stem for p in args.directory.glob('*.json')} == {name for name, _ in matrix},
            'Incomplete or extra depth-only evidence')
    report = dict(schema=1, scope='owned_private_depth_only_merger_validation', completed=False,
                  cases=0, negative_controls=0, files={})
    for name, selection in matrix:
        path = args.directory / (name + '.json')
        raw = path.read_bytes()
        document = json.loads(raw)
        validate(document, *selection)
        report['cases'] += 1
        report['files'][path.name] = hashlib.sha256(raw).hexdigest()
        corruptions = [('backend', 'native-pixel-merger/3'), ('scope', 'original_fragment'),
                       ('original_fragment_eligibility', True), ('original_early_depth_reconstructed', True),
                       ('reference_storage_unchanged', False), ('source_sha256', '0' * 64),
                       ('color_after', [0]), ('depth_after', []), ('sample_count', 0), ('sample_quality', 1),
                       ('private_pixel_location', [0, 0]), ('private_target_extent', [1, 1]),
                       ('logical_payload_bound', 0), ('payload_limit', 1), ('effective_sample_mask', -1),
                       ('depth_view_flags', -1), ('passing_samples', -1),
                       ('reference_entire_draw_passing_samples', -1)]
        for key, value in corruptions:
            bad = copy.deepcopy(document)
            bad['result'][key] = value
            try:
                validate(bad, *selection)
            except ValueError:
                report['negative_controls'] += 1
            else:
                raise ValueError('Accepted invalid depth-only evidence: ' + key)
    report['completed'] = True
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'files'}))


if __name__ == '__main__':
    main()
