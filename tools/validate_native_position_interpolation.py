"""Validate native input preservation against independent original GPU outputs."""
import argparse
import copy
import hashlib
import json
from pathlib import Path

from validate_native_pixel_merger import require


def validate(r, vendor, samples, sample, edge, first, mode):
    sample_frequency = mode in ('sample', 'attribute-sample')
    require(r['schema'] == 1 and r['kind'] == 'native_position_interpolation_probe' and
            r['status'] == 'ok' and r['original_fragment_eligibility'] is None, 'Probe scope and result')
    require([r['vendor_request'], r['samples'], r['sample'], r['edge'], r['position_first'], r['mode']] ==
            [vendor, samples, sample, edge, first, mode], 'Fixture identity')
    require([r['x'], r['y']] == ([7, 8] if edge else [3, 2]), 'Fixture pixel')
    require(r['position_register'] == (0 if first else 1) and
            r['data_register'] == (1 if first else 0) and
            r['position_valid'] == 5 and r['data_valid'] == 3, 'Original register layout and validity')
    p, a = r['position_bits'], r['data_bits']
    require(len(p) == len(a) == 4 and [p[2], a[0], a[1], p[0]] == r['reference'],
            'Independent original GPU bits')
    provenance = r['provenance']
    require(provenance['original_fragment_eligibility'] is None and
            provenance['source'] == 'unique-private-raster-quad/1' and
            provenance['sample_frequency'] is sample_frequency and
            provenance['selected_raster']['depth_bits'] == p[2] and
            provenance['selected_raster']['sample'] == sample, 'Invocation provenance')
    passes = provenance['passes']
    require(len(passes) == 2 and {x['input_register'] for x in passes} == {0, 1}, 'Complete register join')
    indexed = sample_frequency and samples == 4
    for observation in passes:
        require(observation['backend'] == 'dxbc-pixel-raster-inputs/2' and observation['records'] == 1 and
                observation['shader_sha256'] == r['shader_sha256'] and
                observation['sample_index_observed'] is False and
                observation['sample_frequency'] is sample_frequency and
                observation['sample'] == sample and observation['private_draws'] == 1 and
                [observation['x'], observation['y']] == [r['x'], r['y']], 'Observation identity')
        require(observation['adapter_vendor'] == (vendor or 5140) and
                observation['target_samples'] == samples and observation['raster_samples'] == samples,
                'Hardware identity')
        require(observation['valid_mask'] == (5 if observation['input_register'] == r['position_register'] else 3),
                'Pass register validity')
        if indexed:
            indices = observation['selected_storage_records']
            require(observation['sample_identity_source'] == 'integer_storage_sample' and
                    len(indices) == 1 and 0 < indices[0] <= observation['storage_records'] <= samples and
                    observation['selected_storage_record'] == indices[0] and
                    observation['index_target_logical_bytes'] == 16*16*4*4, 'Integer sample mapping')
        else:
            require(observation['sample_identity_source'] == 'shader_selection' and
                    observation['storage_records'] == 1 and observation['selected_storage_record'] is None and
                    observation['selected_storage_records'] == [] and observation['index_target_logical_bytes'] == 0,
                    'Single invocation selection')
    if indexed:
        require(len({p['storage_records'] for p in passes}) == 1, 'Original frequency trigger differs across passes')
        require(r['geometry_shader_reference'] == r['reference'], 'Independent GS control')
        require(len(r['explicit_sample_index_reference']) == 4, 'Explicit sample input control')
    if indexed and sample == 3 and not edge and first:
        observed = r['range_observation']
        indices = observed['selected_storage_records']
        require(observed['records'] == observed['private_draws'] == len(indices) == 2 and
                0 < indices[0] < indices[1] <= observed['storage_records'] and
                observed['sample_identity_source'] == 'integer_storage_sample' and
                r['range_recovery_checks'] >= 8, 'Range mapping and cancellation')
        if 'post_cancel_reference' in r:
            require(r['post_cancel_reference'] == r['reference'] and r['retry_records'] == 2,
                    'Post-cancellation retry and original drawing')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--recovery-directory', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    files = {}
    controls = indexed_cases = hardware_differences = 0
    for vendor in (0, 4318, 32902):
        for samples in (1, 4):
            for sample in range(samples):
                for edge in (False, True):
                    if edge and (samples == 1 or sample not in (0, 2)):
                        continue
                    for first in (False, True):
                        for mode in ('center', 'centroid', 'sample', 'attribute-sample'):
                            path = args.directory / f'{vendor}-{samples}-{sample}-{int(edge)}-{int(first)}-{mode}.json'
                            raw = path.read_bytes()
                            report = json.loads(raw)
                            context = vendor, samples, sample, edge, first, mode
                            validate(report, *context)
                            files[str(path)] = hashlib.sha256(raw).hexdigest()
                            indexed = samples == 4 and mode in ('sample', 'attribute-sample')
                            indexed_cases += indexed
                            if indexed:
                                hardware_differences += report['explicit_sample_index_reference'] != report['reference']
                            changes = [lambda r: r.update(original_fragment_eligibility=True),
                                       lambda r: r.update(position_valid=15),
                                       lambda r: r.update(data_valid=15),
                                       lambda r: r['reference'].__setitem__(0, r['reference'][0] ^ 1),
                                       lambda r: r['position_bits'].__setitem__(0, r['position_bits'][0] ^ 1),
                                       lambda r: r['provenance']['selected_raster'].update(depth_bits=0),
                                       lambda r: r['provenance']['passes'][0].update(sample_index_observed=True),
                                       lambda r: r['provenance']['passes'][0].update(shader_sha256='0'*64),
                                       lambda r: r['provenance']['passes'][0].update(sample=32),
                                       lambda r: r['provenance']['passes'][0].update(storage_records=0)]
                            if indexed:
                                changes += [lambda r: r['provenance']['passes'][0].update(selected_storage_records=[0]),
                                            lambda r: r['provenance']['passes'][0].update(sample_identity_source='shader_selection')]
                            if 'post_cancel_reference' in report:
                                changes += [lambda r: r.update(post_cancel_reference=[]),
                                            lambda r: r.update(retry_records=0)]
                            for change in changes:
                                broken = copy.deepcopy(report)
                                change(broken)
                                try:
                                    validate(broken, *context)
                                except ValueError:
                                    controls += 1
                                else:
                                    raise ValueError('Accepted corrupted position observation: '+str(path))
    require(len(files) == 168 and indexed_cases == 72, 'Complete interpolation matrix')
    recovery_cases = 0
    if args.recovery_directory:
        for vendor in (0, 4318, 32902):
            for mode in ('sample', 'attribute-sample'):
                path = args.recovery_directory / f'{vendor}-4-3-0-1-{mode}.json'
                raw = path.read_bytes()
                report = json.loads(raw)
                require('post_cancel_reference' in report, 'Missing recovery evidence')
                validate(report, vendor, 4, 3, False, True, mode)
                files[str(path)] = hashlib.sha256(raw).hexdigest()
                recovery_cases += 1
                for key, value in (('post_cancel_reference', []), ('retry_records', 0)):
                    broken = copy.deepcopy(report)
                    broken[key] = value
                    try:
                        validate(broken, vendor, 4, 3, False, True, mode)
                    except ValueError:
                        controls += 1
                    else:
                        raise ValueError('Accepted corrupted recovery evidence')
    result = dict(schema=1, completed=True, cases=168, recovery_cases=recovery_cases, indexed_cases=indexed_cases,
                  explicit_input_changed_reference=hardware_differences, negative_controls=controls,
                  complete_pixel_history=False, files=files)
    with args.out.open('x', encoding='utf-8') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'files'}))


if __name__ == '__main__':
    main()
