"""Validate captured-geometry instance partitions without inferring original pixel invocations."""
import argparse
import copy
import hashlib
import json
from pathlib import Path


def require(value, message):
    if not value:
        raise ValueError(message)


def partition(geometry):
    meta = geometry.get('geometry_instances')
    if meta is None:
        return None
    require(meta['schema'] == 1 and meta['domain'] == 'zero_based_draw_instance' and
            meta['scope'] == 'captured_geometry_partition' and
            meta['geometry_sha256'] == geometry['sha256'] and
            meta['primitive_count'] == geometry['primitives'] and
            meta['original_fragment_eligibility'] is None and meta['original_gpu_schedule'] is False,
            'Geometry instance provenance')
    total, count = meta['instance_count'], geometry['primitives']
    require(type(total) is int and 0 <= total <= 0xffffffff and
            total == meta['original_parameters'].get('instance_count', 1), 'Instance count')
    if meta['status'] == 'unavailable':
        require(meta['ranges'] == [] and bool(meta['reason']), 'Unknown partition contains no ranges')
        return None
    require(meta['status'] == 'resolved', 'Instance resolution status')
    if meta['strategy'] == 'uniform_geometry_output':
        require((total == 0 and count == 0) or (total > 0 and count % total == 0), 'Uniform partition extent')
        each = count // total if total else 0
        require(meta['primitives_per_instance'] == each and meta['ranges'] == [] and
                meta['prefix_queries'] == [] and
                (total <= 1 or count == 0 or geometry['executed_stages'] == ['vs']), 'Uniform instance proof')
        return lambda ordinal: ordinal // each
    require(meta['strategy'] == 'native_prefix_statistics' and total > 0 and
            len(meta['ranges']) == total and len(meta['prefix_queries']) == total - 1,
            'Complete prefix instance partition')
    proof = meta['uav_independence']
    if geometry['pre_raster_uav_isolation']['stages']:
        require(proof['input_aliases_excluded'] is True and
                set(proof['stages']) == set(geometry['executed_stages']) and
                all(stage['proved'] is True for stage in proof['stages'].values()), 'UAV independence')
    end, ends = 0, []
    for i, row in enumerate(meta['ranges']):
        require(row['instance'] == i and row['first_primitive'] == end and
                type(row['primitive_count']) is int and 0 <= row['primitive_count'] <= count - end,
                'Contiguous instance range')
        end += row['primitive_count']
        if i + 1 < total:
            query = meta['prefix_queries'][i]
            require(query['instance_count'] == i + 1 and query['primitives'] == end and
                    query['attempts'][-1]['primitives_storage_needed'] == end,
                    'Prefix query agrees with range')
        ends.append(end)
    require(end == count, 'Complete instance coverage')
    return lambda ordinal: next(i for i, end in enumerate(ends) if ordinal < end)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('identity', type=Path)
    parser.add_argument('uav', type=Path)
    parser.add_argument('--histories', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    rows, faults, paths = [], 0, []
    def record(path, geometry):
        nonlocal faults
        mapper = partition(geometry)
        require('geometry_instances' in geometry, 'New instance evidence required')
        row = {'path': path.as_posix(), 'geometry_sha256': geometry['sha256'],
               'status': geometry['geometry_instances']['status'], 'primitives': geometry['primitives']}
        rows.append(row)
        for key, value in [('geometry_sha256', '0' * 64), ('primitive_count', -1),
                           ('domain', 'SV_InstanceID'), ('original_gpu_schedule', True),
                           ('original_fragment_eligibility', True)]:
            bad = copy.deepcopy(geometry)
            bad['geometry_instances'][key] = value
            try:
                partition(bad)
            except ValueError:
                faults += 1
            else:
                raise ValueError('Accepted corrupted partition ' + key)
        meta = geometry['geometry_instances']
        mutations = []
        if meta['status'] == 'resolved' and meta['strategy'] == 'uniform_geometry_output':
            mutations.append(lambda r: r['geometry_instances'].update(primitives_per_instance=-1))
        elif meta['status'] == 'resolved':
            mutations.extend([
                lambda r: r['geometry_instances']['ranges'][0].update(first_primitive=1),
                lambda r: r['geometry_instances']['ranges'][-1].update(primitive_count=-1),
                lambda r: r['geometry_instances']['prefix_queries'][0].update(primitives=-1)])
            if geometry['pre_raster_uav_isolation']['stages']:
                mutations.append(lambda r: r['geometry_instances']['uav_independence'].update(input_aliases_excluded=False))
        else:
            mutations.append(lambda r: r['geometry_instances']['ranges'].append({'instance': 0}))
        for mutate in mutations:
            bad = copy.deepcopy(geometry)
            mutate(bad)
            try:
                partition(bad)
            except ValueError:
                faults += 1
            else:
                raise ValueError('Accepted corrupted partition range or proof')
        return mapper
    topology_count = original_count = uav_count = 0
    for path in sorted(args.identity.glob('*.json')):
        r = json.loads(path.read_text())
        if path.name.startswith('topology-'):
            topology_count += 1
            mapping = record(path, r['geometry'])
            require(mapping is not None, 'VS topology mapping')
            each = r['expected_per_instance']
            require(all(mapping(i) == i // each for i in range(len(r['ids']))), 'Authored VS instance sequence')
            paths.append(path)
        elif path.name.startswith('originalPixel-'):
            original_count += 1
            mapping = record(path, r['geometry'])
            require(mapping is not None and all(mapping(i) == i // 4 for i in range(len(r['ids']))),
                    'Original draw instance sequence')
            paths.append(path)
    require(topology_count == 54 and original_count == 36, 'Complete primitive instance matrix')
    for path in sorted(args.uav.glob('*.json')):
        r = json.loads(path.read_text())
        mode = r['mode']
        mapping = record(path, r['pixel_geometry'])
        rejected = mode == 'read' or 'alias' in mode
        require((mapping is None) == rejected and r['storage_restored'] is True and
                r['counter_restored'] is True and r['clean_draw_equal'] is True, 'Safe mutable geometry partition')
        if not rejected:
            expected = ([1, 2, 2] if mode == 'gs-empty' else [0, 1, 2] if mode == 'gs-unbound'
                        else [0, 1, 1, 2, 2, 2] if mode.startswith('gs') else [0, 0, 1, 1])
            require([mapping(i) for i in range(r['pixel_geometry']['primitives'])] == expected,
                    'Authored variable-output instance sequence')
        uav_count += 1
        paths.append(path)
    require(uav_count == 30, 'Complete mutable-input and empty-instance matrix')
    joins = 0
    if args.histories:
        for path in sorted(args.histories.glob('*.json')):
            r = json.loads(path.read_text())
            for history in r.get('histories', []):
                mapping = record(path, history['geometry_replay']['geometry'])
                for row in history['records']:
                    require(row['geometry_instance'] == (mapping(row['primitive_ordinal']) if mapping else None)
                            and row['instance'] is None and row['original_fragment_eligibility'] is None,
                            'History geometry instance join preserves unknown original invocation')
                    joins += 1
            paths.append(path)
        require(joins > 0, 'History joins observed')
    result = {'schema': 1, 'status': 'ok', 'partitions': rows, 'rejected_corruptions': faults,
              'history_records': joins, 'original_fragment_instances_recovered': False,
              'files': [{'path': p.as_posix(), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                        for p in sorted(set(paths))]}
    with args.out.open('x', encoding='utf-8') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(json.dumps({'partitions': len(rows), 'rejected_corruptions': faults, 'history_records': joins}))


if __name__ == '__main__':
    main()
