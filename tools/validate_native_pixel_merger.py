"""Validate the owned single-sample merger matrix and independent stencil expectations."""
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
            (1, 'native_pixel_merger', 'native-pixel-merger/1', 'private_single_sample_output_merger'),
            'Wrong merger scope or format')
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


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    p.add_argument('--out', required=True, type=Path)
    args = p.parse_args()
    matrix = list(cases())
    require({path.stem for path in args.directory.glob('*.json')} == {name for name, _ in matrix},
            'Incomplete or extra merger evidence')
    report = dict(schema=1, scope='owned_private_pixel_merger_validation', completed=False,
                  cases=0, negative_controls=0, files={})
    for name, selection in matrix:
        path = args.directory / (name + '.json')
        raw = path.read_bytes()
        document = json.loads(raw)
        validate(document, *selection)
        report['cases'] += 1
        report['files'][path.name] = hashlib.sha256(raw).hexdigest()
        for key, value in [('scope', 'original_fragment'), ('original_fragment_eligibility', True),
                           ('original_early_depth_reconstructed', True), ('reference_storage_unchanged', False),
                           ('source_sha256', '0'*64), ('color_after', [])]:
            bad = copy.deepcopy(document)
            bad['result'][key] = value
            try:
                validate(bad, *selection)
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
