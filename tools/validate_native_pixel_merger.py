"""Validate owned merger matrices, sample isolation and independent storage expectations."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct


def require(value, message):
    if not value:
        raise ValueError(message)


def compare(op, a, b):
    return (False, False, a < b, a == b, a <= b, a > b, a != b, a >= b, True)[op]


def validate(document, vendor, family, mode, value, front=True):
    r, q = document['result'], document['quad_outputs']
    require((r['schema'], r['kind'], r['backend'], r['scope']) ==
            (1, 'native_pixel_merger', 'native-pixel-merger/2', 'private_single_sample_output_merger'),
            'Wrong merger scope or format')
    require(r['sample_count'] == 1 and r['sample_quality'] == 0 and
            r['storage_layout'] == 'sample_major_native_texels', 'Wrong single-sample storage layout')
    require(r['original_fragment_eligibility'] is None and r['original_early_depth_reconstructed'] is False,
            'Unproved original fragment execution')
    require(r['reference_storage_unchanged'] and r['debug_validation'] == 'no_errors', 'Invalid isolated execution')
    require(r['adapter'] == q['adapter'] and r['adapter']['vendor'] == (vendor or 5140) and
            r['adapter']['luid'] != 0, 'Wrong adapter')
    source = json.dumps(q, sort_keys=True, ensure_ascii=False, separators=(',', ':')).encode()
    require(r['source_sha256'] == hashlib.sha256(source).hexdigest(), 'Wrong quad source digest')
    require(q['event'] == (100 if family == 'blend' else 1000) and r['lane'] == 0, 'Wrong source event/lane')
    require(r['color_after'] == r['reference_color'] and r['depth_after'] == r['reference_depth'],
            'Original shader GPU readback differs')
    for key in ('color_before', 'color_after', 'depth_before', 'depth_after'):
        require(all(type(v) is int and 0 <= v <= 255 for v in r[key]), 'Invalid storage byte')
    require(len(r['color_before']) == len(r['color_after']) > 0 and
            len(r['depth_before']) == len(r['depth_after']), 'Storage size changed')
    require(r['count_semantics'] == 'd3d11_occlusion_query', 'Query promoted to fragment identity')
    if family == 'depth':
        require(r['front_face'] == front and r['color_format'] == 28, 'Wrong depth fixture selection')
        desc = r['depth_stencil_descriptor']
        require(len(desc) == 13, 'Wrong depth descriptor size')
        before = bytes(r['depth_before'])
        fmt = r['depth_format']
        require(fmt == (value if mode == 'format' else 45), 'Wrong depth format')
        depth = (int.from_bytes(before[:3], 'little') / 0xffffff if fmt == 45 else
                 int.from_bytes(before, 'little') / 0xffff if fmt == 55 else struct.unpack('<f', before)[0])
        incoming = struct.unpack('<f', struct.pack('<I', r['raster_depth_bits']))[0]
        require(incoming == (.5 if mode == 'shader-depth' else 0) and r['stencil_ref'] == (3 if mode == 'read-mask' else 0xa5),
                'Wrong depth/stencil fixture inputs')
        if mode == 'shader-depth':
            field = next(o for o in q['lanes'][0]['outputs'] if o['kind'].startswith('depth'))
            incoming = struct.unpack('<f', struct.pack('<I', field['raw_bits'][0]))[0]
            require(incoming == (.25 if value == 2 else .75), 'Wrong shader depth')
        stencil = before[3] if fmt == 45 else 0
        read_mask, write_mask = desc[4] & 255, (desc[4] >> 8) & 255
        face = desc[5:9] if front else desc[9:13]
        spass = not desc[3] or compare(face[3], r['stencil_ref'] & read_mask, stencil & read_mask)
        dpass = not desc[0] or compare(desc[2], incoming, depth)
        after = bytearray(before)
        if spass and dpass and desc[0] and desc[1] and not r['depth_view_flags'] & 1:
            if fmt == 45:
                after[:3] = round(incoming * 0xffffff).to_bytes(3, 'little')
            elif fmt == 55:
                after[:] = round(incoming * 0xffff).to_bytes(2, 'little')
            else:
                after[:] = struct.pack('<f', incoming)
        if fmt == 45 and desc[3] and not r['depth_view_flags'] & 2:
            op = face[0 if not spass else 1 if not dpass else 2]
            transformed = (0, stencil, 0, r['stencil_ref'] & 255, min(255, stencil + 1),
                           max(0, stencil - 1), stencil ^ 255, (stencil + 1) & 255,
                           (stencil - 1) & 255)[op]
            after[3] = (transformed & write_mask) | (stencil & ~write_mask & 255)
        require(list(after) == r['depth_after'], 'Independent depth/stencil expectation differs')
        require(r['color_after'] == ([255, 0, 0, 255] if spass and dpass else r['color_before']),
                'Independent depth/stencil color rejection differs')
    elif mode == 'logic':
        source = (0xabcdef12, 0x13579bdf, 0xff00ff00, 0x80000001)
        destination = (0x0f0f0f0f, 0x2468ace0, 0x55aa55aa, 0x7fffffff)
        words = []
        for s, d in zip(source, destination):
            words.append((0, 0xffffffff, s, ~s, d, ~d, s & d, ~(s & d), s | d,
                          ~(s | d), s ^ d, ~(s ^ d), s & ~d, ~s & d, s | ~d, ~s | d)[value] & 0xffffffff)
        require(r['color_after'] == list(struct.pack('<4I', *words)), 'Independent logic expectation differs')


def cases():
    for vendor in (0, 4318, 32902):
        for mode in ('source', 'destination'):
            for value in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19):
                yield f'{vendor}-{mode}-{value}', (vendor, 'blend', mode, value)
        for mode, values in [('operation', range(1, 6)), ('logic', range(16)),
                             ('write-mask', (0, 1, 5, 10, 15)), ('slot', (3, 7)), ('sample-mask', (0, 1, 2)),
                             ('shader', range(4)), ('format', (10, 11, 24, 28, 29, 41, 49, 54, 61, 87, 91))]:
            for value in values:
                yield f'{vendor}-{mode}-{value}', (vendor, 'blend', mode, value)
        for front in (True, False):
            for mode, values in [(m, range(1, 9)) for m in ('fail', 'depth-fail', 'pass', 'depth-func', 'stencil-func')] + [
                    ('write-mask', (15,)), ('read-mask', (15,)), ('no-depth-write', (0,)),
                    ('read-only', (1, 2, 3)), ('format', (40, 55)), ('shader-depth', (0, 1, 2))]:
                for value in values:
                    yield f'{vendor}-{int(front)}-{mode}-{value}', (vendor, 'depth', mode, value, front)


def msaa_cases():
    for vendor in (0, 4318, 32902):
        for samples in (2, 4):
            for mode in ('blend', 'write-mask', 'coverage', 'sample-mask', 'unchanged', 'dual',
                         'alpha', 'coverage-output', 'discard', 'sequence', 'depth', 'depth-fail',
                         'stencil', 'read-only-depth', 'read-only-stencil', 'd16', 'd32', 'd32s8',
                         'format-half', 'format-srgb', 'format-bgra', 'format-inherit'):
                for sample in range(samples if mode == 'coverage' else 1):
                    yield f'{vendor}-{samples}-{mode}-{sample}', (vendor, samples, mode, sample)


def validate_msaa(document, vendor, samples, mode, selected):
    r, q = document['result'], document['quad_outputs']
    require((r['schema'], r['kind'], r['backend'], r['scope']) ==
            (1, 'native_pixel_merger', 'native-pixel-merger/2', 'private_multisample_output_merger'),
            'Wrong multisample result identity')
    require(r['sample_count'] == samples and r['sample_quality'] == 0 and
            r['storage_layout'] == 'sample_major_native_texels' and r['private_pixel_location'] == [0, 0],
            'Wrong private multisample coordinates or layout')
    require(r['original_fragment_eligibility'] is None and r['original_early_depth_reconstructed'] is False,
            'Invented original fragment execution')
    require(r['reference_storage_unchanged'] and r['debug_validation'] == 'no_errors', 'Failed state isolation')
    require(r['adapter'] == q['adapter'] and r['adapter']['vendor'] == (vendor or 5140) and
            r['adapter']['luid'] != 0, 'Wrong multisample adapter')
    source = json.dumps(q, sort_keys=True, ensure_ascii=False, separators=(',', ':')).encode()
    require(r['source_sha256'] == hashlib.sha256(source).hexdigest(), 'Wrong source digest')
    depth = mode in ('depth', 'depth-fail', 'stencil', 'read-only-depth', 'read-only-stencil', 'd16', 'd32', 'd32s8')
    require(q['event'] == (1000 if depth else 200 if mode == 'sequence' else 100) and r['lane'] == 0,
            'Wrong source event/lane')
    cf = 28 if depth else {'format-half': 10, 'format-srgb': 29, 'format-bgra': 87}.get(mode, 2)
    df = {'d16': 55, 'd32': 40, 'd32s8': 20}.get(mode, 45) if depth else 0
    require(r['color_format'] == cf and r['depth_format'] == df and r['color_slot'] == 0, 'Wrong storage format')
    cs = {2: 16, 10: 8, 28: 4, 29: 4, 87: 4}[cf]
    ds = {0: 0, 45: 4, 55: 2, 40: 4, 20: 8}[df]
    require(r['color_invalid_byte_offsets'] == [] and r['depth_invalid_byte_offsets'] == ([5, 6, 7] if df == 20 else []),
            'Unknown storage bytes treated as data')
    for field, size in [('color_before', cs), ('color_after', cs), ('depth_before', ds), ('depth_after', ds)]:
        require(len(r[field]) == size * samples and all(type(v) is int and 0 <= v <= 255 for v in r[field]),
                'Invalid sample storage')
    require(r['color_after'] == r['reference_color'] and r['depth_after'] == r['reference_depth'],
            'Original shader comparison differs')
    mask = 0 if mode == 'sample-mask' else 2 if mode == 'sequence' else 0x55555555
    coverage = 1 << selected if mode == 'coverage' else 0xffffffff
    require(r['sample_mask'] == mask and r['coverage_mask'] == coverage and
            r['effective_sample_mask'] == mask & coverage, 'Wrong sample selection')
    require(r['count_semantics'] == 'd3d11_occlusion_query', 'Query misidentified as fragment identity')
    active = mask & coverage & ((1 << samples) - 1)
    if mode == 'coverage-output':
        active &= 3
    if mode in ('discard', 'depth-fail'):
        active = 0
    if mode == 'alpha':
        require(0 <= r['passing_samples'] <= active.bit_count(), 'Alpha-to-coverage count outside supplied coverage')
    else:
        require(r['passing_samples'] == active.bit_count(), 'Wrong native sample count')
    for sample in range(samples):
        before = r['color_before'][sample * cs:(sample + 1) * cs]
        after = r['color_after'][sample * cs:(sample + 1) * cs]
        db = r['depth_before'][sample * ds:(sample + 1) * ds]
        da = r['depth_after'][sample * ds:(sample + 1) * ds]
        enabled = bool(active & (1 << sample))
        if not enabled or mode == 'unchanged':
            require(after == before, 'Unselected sample changed color')
        if not enabled:
            require(da == db, 'Unselected sample changed depth/stencil')
        if df:
            expected = db.copy()
            if enabled:
                if mode != 'read-only-depth':
                    expected[:{45: 3, 55: 2, 40: 4, 20: 4}[df]] = [0] * {45: 3, 55: 2, 40: 4, 20: 4}[df]
                if mode in ('stencil', 'read-only-depth', 'd32s8'):
                    expected[4 if df == 20 else 3] = 0xa5
                require(after == [255, 0, 0, 255], 'Selected sample did not receive original color')
            require(da == expected, 'Independent multisample depth/stencil differs')
        elif cf == 2:
            expected = list(struct.unpack('<4f', bytes(before)))
            source_color = [.5, .25, .75, 0 if mode == 'coverage-output' else .5]
            if enabled and mode != 'unchanged':
                if mode == 'blend':
                    expected = [s * .5 + d * .5 for s, d in zip(source_color[:3], expected[:3])] + [.5]
                elif mode == 'dual':
                    expected = [s * f + d * .5 for s, f, d in zip(source_color[:3], [.25, .75, .5], expected[:3])] + [.5]
                elif mode == 'write-mask':
                    expected[0], expected[2] = source_color[0], source_color[2]
                else:
                    expected = source_color
            encoded = list(struct.pack('<4f', *expected))
            if mode == 'alpha' and enabled:
                require(after in (encoded, before), 'Alpha-to-coverage sample has an invented value')
            else:
                require(after == encoded, 'Independent float multisample merger differs')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--msaa', action='store_true')
    args = p.parse_args()
    matrix = list(msaa_cases() if args.msaa else cases())
    validator = validate_msaa if args.msaa else validate
    require({path.stem for path in args.directory.glob('*.json')} == {name for name, _ in matrix},
            'Incomplete or extra merger evidence')
    report = dict(schema=1, scope='owned_private_pixel_merger_validation', completed=False,
                  cases=0, negative_controls=0, files={})
    for name, selection in matrix:
        path = args.directory / (name + '.json')
        raw = path.read_bytes()
        document = json.loads(raw)
        validator(document, *selection)
        report['cases'] += 1
        report['files'][path.name] = hashlib.sha256(raw).hexdigest()
        corruptions = [('scope', 'original_fragment'), ('original_fragment_eligibility', True),
                           ('original_early_depth_reconstructed', True), ('reference_storage_unchanged', False),
                           ('source_sha256', '0'*64), ('color_after', [])]
        if args.msaa:
            corruptions += [('sample_count', 1), ('sample_quality', 1), ('private_pixel_location', [1, 1]),
                            ('effective_sample_mask', -1), ('depth_invalid_byte_offsets', [0])]
        for key, value in corruptions:
            bad = copy.deepcopy(document)
            bad['result'][key] = value
            try:
                validator(bad, *selection)
            except ValueError:
                report['negative_controls'] += 1
            else:
                raise ValueError('Accepted invalid merger evidence: ' + key)
    report['completed'] = True
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'files'}))


if __name__ == '__main__':
    main()
