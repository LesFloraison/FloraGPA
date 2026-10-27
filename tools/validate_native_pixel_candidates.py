"""Validate the complete owned native candidate coverage matrix and negative controls."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct


def require(value, message):
    if not value:
        raise ValueError(message)


def geometry_hash():
    data = bytearray()
    for primitive in range(6):
        for vertex in range(3):
            x = 4 if primitive == 2 else -1 if primitive == 5 else 3 if vertex == 2 else -1
            y = -1 if primitive == 5 else 3 if vertex == 1 else -1
            data += struct.pack('<5fI', x, y, .5, 1, -1 if primitive == 3 else 1, int(primitive == 4))
    return hashlib.sha256(data).hexdigest()


def validate(r, vendor, samples, layer, sample, mode):
    require((r['schema'], r['kind'], r['backend']) ==
            (1, 'native_pixel_candidates', 'native-pixel-candidates/1'), 'Wrong candidate format')
    require(r['scope'] == 'captured_geometry_pre_depth_coverage' and r['order'] == 'unspecified', 'Wrong scope/order')
    require(r['original_fragment_eligibility'] is None, 'Candidate promoted to eligible fragment')
    for key in ('original_depth_stencil_executed', 'original_pixel_shader_executed',
                'original_predicate_executed', 'original_resources_accessed'):
        require(r[key] is False, 'Original execution claimed')
    require(r['geometry_sha256'] == geometry_hash(), 'Source geometry changed')
    first, count = (1, 4) if mode == 'range' else (0, 6)
    require((r['first_primitive'], r['primitive_count'], r['x'], r['y'], r['sample'], r['layer']) ==
            (first, count, 4, 4, sample, layer), 'Selection changed')
    require(r['sample_selection_domain'] == 'raster_coverage' and r['raster_passes'] == 1, 'Pass domain changed')
    expected = []
    reference = []
    for ordinal in range(first, first + count):
        covers = mode not in ('scissor', 'mask') and (ordinal in (0, 1) if layer == 0 else ordinal == 4)
        reference.append(dict(ordinal=ordinal, count=int(covers)))
        if covers:
            expected.append(ordinal)
    require(r['reference_counts'] == reference, 'Independent reference changed')
    require(r['unique_primitive_ordinals'] == expected, 'Candidate identity changed')
    rows = r['observations']
    require(len(rows) == len(expected) and sorted(row['primitive_ordinal'] for row in rows) == expected,
            'Candidate record multiplicity changed')
    require({row['observation_index'] for row in rows} == set(range(len(rows))), 'Record index changed')
    for row in rows:
        require(all(row[k] is None for k in ('instance', 'original_primitive_id', 'original_fragment_eligibility')),
                'Unknown identity was invented')
        require(0 <= row['selected_lane'] < 4 and len(row['quad_coverage']) == 4 and
                row['quad_coverage'][row['selected_lane']] & (1 << sample), 'Missing sample coverage')
    capture = r['capture']
    require(capture['scope'] == 'isolated_geometry_range_pre_depth_inputs' and
            capture['original_fragment_eligibility'] is False, 'Wrong capture provenance')
    require(capture['adapter_vendor'] == (vendor or 5140) and capture['warp'] == (not vendor), 'Wrong capture device')
    require(capture['adapter_device'] == r['adapter']['adapter']['device_id'] and
            capture['adapter_vendor'] == r['adapter']['adapter']['vendor_id'], 'Reference device differs')
    require(capture['target_samples'] == capture['raster_samples'] == samples and
            capture['records'] == len(rows), 'Capture shape changed')
    require(capture['geometry']['primitive_count'] == count and capture['geometry']['version'] == 2,
            'Wrong bridge range')
    require(r['geometry_metadata']['vertices'] == 18 and r['geometry_metadata']['stride'] == 24,
            'Wrong source geometry layout')
    return len(reference), len(rows)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    matrix = []
    for vendor in (0, 4318, 32902):
        for samples in (1, 4):
            for layer in (0, 1):
                for sample in range(samples):
                    for mode in ('ordinary', 'scissor', 'mask', 'range'):
                        matrix.append((vendor, samples, layer, sample, mode))
    names = {'-'.join(map(str, case)) + '.json' for case in matrix}
    require({p.name for p in args.directory.glob('*.json')} == names, 'Incomplete or extra matrix evidence')
    report = dict(schema=1, scope='owned_native_pixel_candidate_validation', completed=False,
                  cases=0, independent_draws=0, observed_candidates=0, negative_controls=0, files={})
    for case in matrix:
        path = args.directory / ('-'.join(map(str, case)) + '.json')
        r = json.loads(path.read_text(encoding='utf-8-sig'))
        draws, candidates = validate(r, *case)
        report['cases'] += 1
        report['independent_draws'] += draws
        report['observed_candidates'] += candidates
        report['files'][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
        for key, value in (('geometry_sha256', '0'*64), ('raster_passes', 2), ('x', 5),
                           ('sample_selection_domain', 'output_storage'), ('original_fragment_eligibility', True),
                           ('order', 'primitive_order'), ('original_pixel_shader_executed', True),
                           ('unique_primitive_ordinals', [100])):
            bad = copy.deepcopy(r)
            bad[key] = value
            try:
                validate(bad, *case)
            except ValueError:
                report['negative_controls'] += 1
            else:
                raise ValueError('Accepted corrupt candidate: ' + key)
    report['completed'] = True
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'files'}))


if __name__ == '__main__':
    main()
