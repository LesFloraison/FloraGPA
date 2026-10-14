"""Development-only post-VS projection; a candidate search, never GPU coverage proof."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


def require(value, message):
    if not value:
        raise ValueError(message)


def uint(value, maximum):
    return type(value) is int and 0 <= value <= maximum


def finite(value):
    return type(value) in (int, float) and math.isfinite(value)


def project(report, vertices, bundle, x, y, radius=1.5, sample_offsets=()):
    require(report.get('completed') is True and bundle.get('completed') is True,
            'Incomplete input')
    require(report['frame_sha256'] == bundle['frame_sha256'] and
            report['event'] == bundle['event'] and
            report['shader_sha256'] == bundle['vs']['shader']['sha256'],
            'Export identities differ')
    require(uint(x, 65535) and uint(y, 65535), 'Invalid pixel coordinate')
    require(finite(radius) and 0 <= radius <= 64, 'Invalid search radius')
    count, stride = report['vertex_count'], report['stride']
    require(uint(count, 1000000) and count > 0 and count % 3 == 0 and
            uint(stride, 2048) and stride >= 16 and stride % 4 == 0,
            'Invalid vertex layout')
    require(bundle['topology'] == 4 and bundle['draw'][0] == count and
            report['primitives'] == count // 3, 'Expected complete triangle list')
    require(len(vertices) == count * stride and len(vertices) <= 256 * 1024 * 1024 and
            hashlib.sha256(vertices).hexdigest() == report['bytes_sha256'],
            'Vertex storage differs')
    positions = [a for a in report['attributes'] if a['semantic'].upper() == 'SV_POSITION']
    require(len(positions) == 1, 'Expected one position output')
    position = positions[0]
    offset = position['offset']
    require(position['index'] == 0 and position['component_type'] == 3 and
            position['count'] == 4 and position['start'] == 0 and
            position['never_written_mask'] == 0 and uint(offset, stride - 16) and
            offset % 4 == 0, 'Unsupported position signature')
    viewports = bundle['viewports']
    require(len(viewports) == 1 and len(viewports[0]) == 6 and
            all(uint(v, 2**32 - 1) for v in viewports[0]), 'Expected one encoded viewport')
    vx, vy, width, height, near, far = struct.unpack('<6f', struct.pack('<6I', *viewports[0]))
    require(all(math.isfinite(v) for v in (vx, vy, width, height, near, far)) and
            0 < width <= 32768 and 0 < height <= 32768 and 0 <= near <= far <= 1,
            'Unsupported viewport')
    require(len(sample_offsets) <= 8 and all(len(p) == 2 and
            all(finite(v) and -0.5 <= v <= 0.5 for v in p) for p in sample_offsets),
            'Invalid sample offsets')
    points = {'center': (x + 0.5, y + 0.5)}
    points.update({f'sample-{i}': (x + 0.5 + p[0], y + 0.5 + p[1])
                   for i, p in enumerate(sample_offsets)})
    candidates = []
    for primitive in range(count // 3):
        clip = [struct.unpack_from('<4f', vertices, (primitive * 3 + i) * stride + offset)
                for i in range(3)]
        require(all(all(math.isfinite(v) for v in p) and p[3] > 0 for p in clip),
                'Nonfinite position or nonpositive clip W requires clipping analysis')
        screen = [[vx + (p[0] / p[3] + 1) * width / 2,
                   vy + (1 - p[1] / p[3]) * height / 2] for p in clip]
        require(all(math.isfinite(v) for p in screen for v in p), 'Nonfinite projection')
        if (min(p[0] for p in screen) > x + 0.5 + radius or
                max(p[0] for p in screen) < x + 0.5 - radius or
                min(p[1] for p in screen) > y + 0.5 + radius or
                max(p[1] for p in screen) < y + 0.5 - radius):
            continue
        ax, ay = screen[1][0] - screen[0][0], screen[1][1] - screen[0][1]
        bx, by = screen[2][0] - screen[0][0], screen[2][1] - screen[0][1]
        determinant = ax * by - ay * bx
        weights = {}
        for label, point in points.items():
            if determinant == 0:
                weights[label] = None
            else:
                px, py = point[0] - screen[0][0], point[1] - screen[0][1]
                u, v = (px * by - py * bx) / determinant, (ax * py - ay * px) / determinant
                weights[label] = [1 - u - v, u, v]
        require(math.isfinite(determinant) and all(w is None or all(math.isfinite(v) for v in w)
                for w in weights.values()), 'Nonfinite barycentric projection')
        candidates.append(dict(primitive=primitive,first_index=primitive * 3,index_count=3,
                               clip=clip,screen=screen,signed_double_area=determinant,
                               area=abs(determinant) / 2,barycentric=weights))
    return dict(schema=1,acceptance_established=False,event=report['event'],pixel=[x,y],
                search_radius=radius,points=points,candidates=candidates,
                scope='Bounding-box candidates from unsnapped post-VS projection. No clipping, culling, '
                      'subpixel, edge-rule, depth, discard or GPU-coverage implementation.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('postvs', type=Path)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('x', type=int)
    parser.add_argument('y', type=int)
    parser.add_argument('output', type=Path)
    parser.add_argument('--radius', type=float, default=1.5)
    parser.add_argument('--sample-report', type=Path)
    args = parser.parse_args()
    require(not args.output.exists(), 'Output already exists')
    report = json.loads((args.postvs / 'report.json').read_text())
    bundle_bytes = args.bundle.read_bytes()
    bundle = json.loads(bundle_bytes)
    require((args.postvs / 'vertices.bin').stat().st_size <= 256 * 1024 * 1024,
            'Vertex storage exceeds bound')
    samples = ()
    if args.sample_report:
        observed = json.loads(args.sample_report.read_text())
        require(observed['completed'] is True and observed['event'] == report['event'] and
                observed['bundle_sha256'] == hashlib.sha256(bundle_bytes).hexdigest() and
                observed['pixel'][1:] == [args.x, args.y], 'Sample report identity differs')
        samples = observed['sample_positions']
        require(len(samples) == len(bundle['resources'][str(observed['pixel'][0])]['samples']),
                'Sample count differs')
    result = project(report, (args.postvs / 'vertices.bin').read_bytes(), bundle,
                     args.x, args.y, args.radius, samples)
    result['source'] = dict(postvs_sha256=hashlib.sha256((args.postvs/'report.json').read_bytes()).hexdigest(),
                          bundle_sha256=hashlib.sha256(bundle_bytes).hexdigest(),
                          sample_report_sha256=hashlib.sha256(args.sample_report.read_bytes()).hexdigest()
                          if args.sample_report else None)
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write('\n')


if __name__ == '__main__':
    main()
