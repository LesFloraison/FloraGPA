"""Check owned primitive identity evidence against topology and original PS observations."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct


def require(value, message):
    if not value:
        raise ValueError(message)


def identity(geometry, values):
    meta = geometry['primitive_identity']
    require(meta['schema'] == 1 and meta['status'] == 'observed', 'Identity not observed')
    require(meta['primitive_count'] == geometry['primitives'] == len(values), 'Identity count mismatch')
    require(meta['geometry_sha256'] == geometry['sha256'], 'Geometry identity mismatch')
    require(meta['pipeline_order'] == 'selected_stream_output_order' and
            meta['original_gpu_schedule'] is False and meta['instance'] is None, 'Invented scheduling/instance')
    raw = struct.pack('<' + 'I' * len(values), *values)
    require(meta['ids_sha256'] == hashlib.sha256(raw).hexdigest(), 'Primitive ID digest mismatch')
    require(meta['source'] in ('native_system_primitive_id_stream_output',
                               'native_domain_patch_id_stream_output',
                               'original_gs_leading_vertex_output'), 'Unexpected identity source')


def validate(report, kind, parameters):
    require(report['status'] == 'ok', 'Unsuccessful case')
    if kind == 'unknown':
        require(len(report['cases']) == 3, 'Missing unknown case')
        for g, reason in zip(report['cases'], (
                'separate_identity_pass_requires_one_stream_without_pre_raster_uavs',
                'original_geometry_shader_does_not_emit_primitive_id',
                'separate_identity_pass_requires_one_stream_without_pre_raster_uavs')):
            m = g['primitive_identity']
            require(m['status'] == 'unavailable' and m['source'] is None and m['reason'] == reason,
                    'Unknown identity was guessed')
        return 0
    if kind == 'boundaries':
        require(report['faults_rejected'] == 9 and report['budget_rejected'] is True, 'Boundary checks missing')
        identity(report['empty'], [])
        return 0
    if kind == 'streamOutput':
        require(len(report['cases']) == 2, 'Missing SO case')
        for row, provider in zip(report['cases'], (False, True)):
            require(row['signature_provider'] is provider and row['ids'] == ([0] if provider else [0, 1]), 'SO identity differs')
            identity(row['geometry'], row['ids'])
        return 0
    g, values = report['geometry'], report['ids']
    identity(g, values)
    if kind == 'topology':
        _, topology, instances = parameters
        count = {1: 24, 2: 12, 3: 23, 4: 8, 5: 22, 10: 6, 11: 21, 12: 4, 13: 10}[topology]
        require(values == list(range(count)) * instances, 'System topology/instance identity differs')
        require(report['expected_per_instance'] == count and report['instances'] == instances,
                'Topology selection differs')
        return 0
    rows = report['original_pixels']
    require(len(rows) == len(values), 'Original pixel evidence missing')
    for i, row in enumerate(rows):
        require(row['ordinal'] == i and row['original_ps_value'] == values[i] + 1,
                'Original PS disagrees with identity')
    if kind == 'originalPixel':
        _, draw, gs = parameters
        expected = [100 + i * 7 if gs else i for i in range(4)] * (1 if draw in (55, 57) else 2)
        require(values == expected, 'Draw identity differs')
        require(g['primitive_identity']['source'] == ('original_gs_leading_vertex_output' if gs else
                'native_system_primitive_id_stream_output'), 'Draw identity source differs')
        require(report['bridge']['version'] == 3 and report['bridge_observed_id'] == values[0],
                'Bridge observed another ID')
        candidate = report['candidates']
        require(candidate['order'] == 'unspecified' and candidate['original_fragment_eligibility'] is None,
                'Candidate eligibility/order invented')
        require(len(candidate['observations']) == 1 and
                candidate['observations'][0]['primitive_ordinal'] == 0 and
                candidate['observations'][0]['original_primitive_id'] == values[0], 'Candidate ID join differs')
    else:
        require(kind == 'tessellation', 'Unexpected case')
        require(g['primitive_identity']['source'] == 'native_domain_patch_id_stream_output', 'Wrong patch ID source')
        require(len(values) == report['reference_clipper_primitives'] and len(values) > 4 and len(values) % 4 == 0,
                'Tessellation statistics differ')
        n = len(values) // 4
        require(values == ([0] * n + [1] * n) * 2, 'Tessellated primitives do not retain patch IDs')
    return len(rows)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--real-root', type=Path)
    p.add_argument('--standalone-root', type=Path)
    a = p.parse_args()
    cases = []
    for vendor in (0, 4318, 32902):
        cases += [('topology', (vendor, t, n)) for t in (1, 2, 3, 4, 5, 10, 11, 12, 13) for n in (1, 2)]
        cases += [('originalPixel', (vendor, d, gs)) for d in (55, 57, 58, 59, 60, 61) for gs in (0, 1)]
        cases += [('tessellation', (vendor, domain)) for domain in ('tri', 'quad', 'isoline')]
        cases += [(kind, (vendor,)) for kind in ('unknown', 'boundaries', 'streamOutput')]
    names = {kind + '-' + '-'.join(map(str, args)) + '.json' for kind, args in cases}
    require({f.name for f in a.directory.glob('*.json')} == names, 'Incomplete or extra identity evidence')
    result = dict(schema=1, scope='owned_native_primitive_identity', cases=0, original_pixel_checks=0,
                  negative_controls=0, files={})
    for kind, args in cases:
        name = kind + '-' + '-'.join(map(str, args)) + '.json'
        raw = (a.directory / name).read_bytes()
        r = json.loads(raw)
        result['original_pixel_checks'] += validate(r, kind, args)
        result['cases'] += 1
        result['files'][name] = hashlib.sha256(raw).hexdigest()
        for fault in range(4 if 'ids' in r else 1):
            bad = copy.deepcopy(r)
            if fault == 0:
                bad['status'] = 'failed'
            elif fault == 1:
                bad['ids'][0] ^= 1
            elif fault == 2:
                bad['geometry']['primitive_identity']['original_gpu_schedule'] = True
            elif fault == 3:
                bad['geometry']['primitive_identity']['instance'] = 0
            try:
                validate(bad, kind, args)
            except ValueError:
                result['negative_controls'] += 1
            else:
                raise ValueError('Accepted corrupt identity evidence')
    if a.real_root:
        result['real_cases'] = []
        captures = dict(gf2=(113, 'fc7b7f7de20a578da2977b2ff91d96f5cf80887b393bc85859be8cab1af91c8e'),
                        bf1=(2010, '26044d33cb6c3087c2204b7d57664203741a679f988430b7d05b24af582e0620'),
                        hd=(200, '97915a80c7617e9a67068a5d85b87f1b2a7883faa9831428084ebd7270f37139'))
        for game, (event, frame_hash) in captures.items():
            for vendor in (0, 4318, 32902):
                for kind in ('candidates', 'outputs'):
                    directory = a.real_root / f'pixel-primitive-identity-real-{game}-{vendor}-{kind}'
                    r = json.loads((directory / 'report.json').read_bytes())
                    g = r['geometry_metadata'] if kind == 'candidates' else json.loads((directory / 'source-geometry.json').read_bytes())
                    raw = (directory / 'primitive-ids.bin').read_bytes()
                    require(len(raw) % 4 == 0, 'Truncated real primitive IDs')
                    values = list(struct.unpack('<' + 'I' * (len(raw) // 4), raw))
                    identity(g, values)
                    require(hashlib.sha256((directory / 'source-geometry.bin').read_bytes()).hexdigest() == g['sha256'],
                            'Real geometry bytes differ')
                    require(r['event'] == event and r['frame_sha256'] == frame_hash, 'Different real capture/event')
                    device = r['capture'] if kind == 'candidates' else r
                    require(device['adapter_vendor'] == (vendor or 5140), 'Different real adapter')
                    if kind == 'candidates':
                        require(r['original_fragment_eligibility'] is None and r['order'] == 'unspecified',
                                'Original candidate eligibility/order invented')
                        for row in r['observations']:
                            require(row['original_primitive_id'] == values[row['primitive_ordinal']], 'Real candidate identity differs')
                    else:
                        require(r['status'] == 'ok' and r['original_fragment_eligibility'] is False, 'Real PS output failed or promoted')
                    result['real_cases'].append(dict(name=directory.name, primitives=len(values), elapsed_ms=r['elapsed_ms'],
                                                     observations=len(r['observations']) if kind == 'candidates' else None))
    if a.standalone_root:
        result['standalone_cases'] = []
        for adapter, vendor in (('warp', 5140), ('nvidia', 4318), ('intel', 32902)):
            for domain, mode in (('tri', 'legacy'), ('isoline', 'legacy'), ('isoline', 'quadrilateral')):
                name = f'pixel-primitive-identity-standalone-{adapter}-{domain}-{mode}'
                if adapter == 'warp' and mode == 'legacy':
                    name = 'pixel-primitive-identity-standalone-warp-' + ('tri' if domain == 'tri' else 'line')
                r = json.loads((a.standalone_root / name / 'report.json').read_bytes())
                require(r['scope'] == 'standalone_d3d11_identity_observation' and not r['debug_errors'], 'Invalid standalone observation')
                require(r['vendor'] == vendor and r['domain'] == domain and r['quadrilateral_lines'] == (mode == 'quadrilateral'),
                        'Different standalone adapter/configuration')
                n = 6 if domain == 'tri' else 4
                require(r['patch_ids'] == ([0] * n + [1] * n) * 2, 'Standalone patch IDs differ')
                require(r['system_matches_patch'] == (r['system_ids'] == r['patch_ids']), 'Incorrect standalone comparison')
                require(r['system_matches_patch'] == (adapter != 'warp'), 'Local driver observation changed')
                expected = 144 if domain == 'tri' else 0 if adapter == 'warp' and mode == 'legacy' else 64
                require(len(r['original_pixels']) == expected, 'Local original raster observation changed')
                require(all(row[2] in (1, 2) for row in r['original_pixels']), 'Original PS patch IDs differ')
                result['standalone_cases'].append(dict(name=name, original_pixels=expected, system_matches_patch=r['system_matches_patch']))
    result['completed'] = True
    with a.out.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'files'}))


if __name__ == '__main__':
    main()
