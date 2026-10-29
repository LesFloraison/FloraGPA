"""Verify owned sparse-position UI evidence against independently packed bytes."""
import argparse
import copy
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import struct


def require(value, message):
    if not value:
        raise ValueError(message)


def same(a, b):
    return a == b and (a != 0 or math.copysign(1, a) == math.copysign(1, b))


def check(name, files):
    layout, special = name.split('-special-')
    indices = ['xyzw'.index(c) for c in layout]
    source = [(2., 4., 6., 2.), (-0., 1., 2., 0.),
              (math.inf if int(special) else 3., 2., 1., 1.)]
    expected = bytearray([0xa5] * 72)
    for row, values in enumerate(source):
        for column, axis in enumerate(indices):
            struct.pack_into('<f', expected, row * 24 + 4 + 4 * column, values[axis])
    require(files['vertices.bin'] == files['post_vertices.bin'] == expected, 'Exact packed storage')
    job, result, presentation = (json.loads(files[key]) for key in ('job.json', 'result.json', 'presentation.json'))
    require(result['data']['attributes'][0]['components'] == indices, 'Source channel identity')
    require(result['data']['sha256'] == hashlib.sha256(expected).hexdigest(), 'Storage digest')
    context = {k: v for k, v in job.items() if k != 'request_id'}
    require(context == result['context'], 'Analysis context')
    digest = hashlib.sha256(json.dumps(context, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    require(digest == result['context_id'], 'Context digest')
    require({k: v for k, v in presentation.items() if k != 'presentation'} == result, 'Exported analysis')
    metadata = presentation['presentation']['replay_mesh']
    require(metadata['position_components'] == indices and metadata['vertex_sources'] == [0, 1, 2], 'Presentation mapping')
    rows = list(csv.reader(io.StringIO(files['post_vertices.csv'].decode())))
    require(rows[0] == ['vertex'] + list(layout) and len(rows) == 4, 'Named CSV columns')
    for row, values in enumerate(source):
        require(rows[row + 1][0] == str(row) and len(rows[row + 1]) == len(layout) + 1, 'CSV row identity')
        require(all(same(float(cell), values[axis]) for cell, axis in zip(rows[row + 1][1:], indices)), 'CSV values')
    xyz = all(c in layout for c in 'xyz')
    preview = json.loads(files['preview.json'])
    if xyz and not int(special):
        require(preview.get('faces') == [[1, 2, 3]] and len(preview.get('positions', [])) == 3, 'Preview topology')
        for row, values in zip(preview['positions'], source):
            w = values[3] if 'w' in layout and values[3] else 1
            require(len(row) == 3 and all(same(a, b / w) for a, b in zip(row, values[:3])), 'Semantic preview coordinates')
    else:
        require(preview == {}, 'Unknown or nonfinite preview must be absent')
    obj = xyz or 'xyzw'.startswith(layout)
    require(('post_geometry.obj' in files) == obj, 'OBJ availability')
    require((metadata['obj_unavailable_reason'] is None) == obj, 'OBJ availability reason')
    if obj:
        lines = files['post_geometry.obj'].decode().splitlines()
        vertices = [line.split()[1:] for line in lines if line.startswith('v ')]
        require(len(vertices) == 3 and [line for line in lines if line.startswith('f ')] == ['f 1 2 3'], 'OBJ topology')
        for row, values in zip(vertices, source):
            w = values[3] if 'w' in layout and values[3] else 1
            count = min(3, len(layout))
            require(len(row) == count and all(same(float(a), b / w) for a, b in zip(row, values[:count])), 'OBJ semantic axes')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    layouts = [''.join(c for i, c in enumerate('xyzw') if mask & 1 << i) for mask in range(1, 16)] + ['zxy', 'wzxy']
    names = {f'{layout}-special-{special}' for layout in layouts for special in (0, 1)}
    require({p.name for p in args.root.iterdir() if p.is_dir()} == names, 'Complete channel matrix')
    rejected = 0
    for name in sorted(names):
        files = {p.name: p.read_bytes() for p in (args.root / name).iterdir() if p.is_file()}
        check(name, files)
        altered = copy.deepcopy(files)
        altered['post_vertices.bin'] = bytes([0]) + altered['post_vertices.bin'][1:]
        mutations = [altered]
        altered = copy.deepcopy(files)
        altered['post_vertices.csv'] = altered['post_vertices.csv'].replace(b'vertex,', b'vertex,unknown,', 1)
        mutations.append(altered)
        altered = copy.deepcopy(files)
        presentation = json.loads(altered['presentation.json'])
        presentation['presentation']['replay_mesh']['position_components'] = [4]
        altered['presentation.json'] = json.dumps(presentation).encode()
        mutations.append(altered)
        altered = copy.deepcopy(files)
        altered['preview.json'] = b'{}' if json.loads(altered['preview.json']) else b'{"positions":[[0,0,0]]}'
        mutations.append(altered)
        altered = copy.deepcopy(files)
        altered['post_geometry.obj'] = b''
        mutations.append(altered)
        altered = copy.deepcopy(files)
        result = json.loads(altered['result.json'])
        result['data']['attributes'][0]['components'] = [4]
        altered['result.json'] = json.dumps(result).encode()
        mutations.append(altered)
        for altered in mutations:
            try:
                check(name, altered)
            except ValueError:
                rejected += 1
            else:
                raise ValueError('Corrupted presentation accepted')
    result = dict(schema=1, status='verified', cases=len(names), channel_layouts=len(layouts),
                  corruptions_rejected=rejected, scope='owned_storage_and_ui', gpu_sparse_signature_qualified=False,
                  full_mesh_parity=False)
    args.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
