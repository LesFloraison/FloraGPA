"""Validate measured depth/front-face inputs against independent D3D11 pixel output."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct


def require(condition, message):
    if not condition:
        raise ValueError(message)


def cases():
    for vendor in (0, 4318, 32902):
        for samples in (1, 4):
            for reverse in (0, 1):
                for sloped in (0, 1):
                    for mode in ('constant', 'position', 'front', 'sample'):
                        yield f'{vendor}-{samples}-{reverse}-{sloped}-{mode}', (vendor, samples, reverse, sloped, mode)


def validate(r, vendor, samples, reverse, sloped, mode):
    require(r['schema'] == 1 and r['kind'] == 'private_raster_merger_inputs_probe', 'Wrong report identity')
    require([r['vendor_request'], r['samples'], r['reverse'], r['sloped'], r['mode']] ==
            [vendor, samples, bool(reverse), bool(sloped), mode], 'Wrong test selection')
    require(r['original_fragment_eligibility'] is None, 'Invented original eligibility')
    p = r['provenance']
    require(p['source'] == 'unique-private-raster-quad/1' and
            p['scope'] == 'isolated_primitive_pre_depth_inputs' and p['original_fragment_eligibility'] is None,
            'Wrong observation scope')
    require(p['sample_frequency'] == (mode == 'sample') and p['coverage_only'] == (mode == 'constant'),
            'Wrong invocation frequency/input coverage')
    reference = r['reference']
    require(len(reference) == 4 and all(type(v) is int and 0 <= v <= 0xffffffff for v in reference),
            'Invalid reference words')
    require(reference[1:] == [int(not reverse), (1 << samples) - 1, samples - 1 if mode == 'sample' else 0],
            'Independent reference orientation/coverage/sample differs')
    depth = struct.unpack('<f', struct.pack('<I', reference[0]))[0]
    require(.2 <= depth <= .8 and (sloped or abs(depth - .35) < 1e-7), 'Independent reference depth differs')
    observed = p['selected_raster']
    require(observed['depth_bits'] == reference[0] and observed['front_face'] == bool(reference[1]) and
            observed['coverage_mask'] == reference[2] and observed['sample'] == samples - 1 and
            type(observed['lane']) is int and 0 <= observed['lane'] < 4, 'Measured merger inputs differ')
    require(p['passes'], 'Missing observation passes')
    for entry in p['passes']:
        require(entry['backend'] == 'dxbc-pixel-raster-inputs/2' and entry['records'] == 1 and
                entry['record_stride'] == 144 and entry['original_fragment_eligibility'] is False,
                'Wrong source record version/count/scope')
        require(entry['shader_sha256'] == r['original_shader_sha256'], 'Wrong original shader')
        require(entry['adapter_vendor'] == (vendor or 5140) and entry['adapter_luid'] != 0, 'Wrong adapter')
        require([entry[k] for k in ('x', 'y', 'sample', 'layer')] == [3, 2, samples - 1, 0] and
                entry['target_samples'] == entry['raster_samples'] == samples and
                entry['sample_frequency'] == (mode == 'sample'), 'Wrong source raster selection')
        require(entry['raster_state']['rasterizer']['front_ccw'] == reverse, 'Wrong raster orientation')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    matrix = list(cases())
    require({p.stem for p in args.directory.glob('*.json')} == {n for n, _ in matrix}, 'Incomplete matrix')
    report = dict(schema=1, scope='private_raster_merger_inputs_validation', completed=False,
                  cases=0, negative_controls=0, files={})
    for name, selection in matrix:
        path = args.directory / (name + '.json')
        raw = path.read_bytes()
        r = json.loads(raw)
        validate(r, *selection)
        report['cases'] += 1
        report['files'][path.name] = hashlib.sha256(raw).hexdigest()
        mutations = [(['original_fragment_eligibility'], True), (['vendor_request'], -1),
                     (['provenance', 'selected_raster', 'depth_bits'], 0),
                     (['provenance', 'selected_raster', 'front_face'], bool(r['reverse'])),
                     (['provenance', 'selected_raster', 'coverage_mask'], 0),
                     (['provenance', 'selected_raster', 'sample'], -1),
                     (['provenance', 'selected_raster', 'lane'], 4),
                     (['provenance', 'passes', 0, 'backend'], 'dxbc-pixel-raster-inputs/1'),
                     (['provenance', 'passes', 0, 'adapter_luid'], 0),
                     (['provenance', 'passes', 0, 'shader_sha256'], '0' * 64)]
        for keys, value in mutations:
            bad = copy.deepcopy(r)
            item = bad
            for key in keys[:-1]:
                item = item[key]
            item[keys[-1]] = value
            try:
                validate(bad, *selection)
            except ValueError:
                report['negative_controls'] += 1
            else:
                raise ValueError('Accepted invalid measured raster input: ' + str(keys))
    report['completed'] = True
    with args.out.open('x', encoding='utf-8') as output:
        json.dump(report, output, indent=2)
        output.write('\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'files'}))


if __name__ == '__main__':
    main()
