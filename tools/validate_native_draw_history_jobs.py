"""Validate native private-draw Worker exports and their owned GPU evidence."""
import argparse
import copy
import hashlib
import json
import struct
from pathlib import Path

from validate_native_pixel_draw_history import validate as storage
from validate_native_pixel_history_shader import validate as shader
from validate_native_pixel_history_merger import validate as merger
from validate_native_pixel_identity import validate_history_identity


def require(value, message):
    if not value:
        raise AssertionError(message)


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
                                     separators=(',', ':')).encode()).hexdigest()


def validate(job, result, vendor):
    require(result['schema'] == job['schema'] == 1 and result['backend'] == job['backend'] and
            result['ok'] is True and result['request_id'] == job['request_id'], 'Job identity')
    context, data = result['context'], result['data']
    for key in ('schema', 'backend', 'backend_sha256', 'frame_sha256', 'experiment_sha256', 'action', 'options'):
        require(context[key] == job[key], 'Stale job context: ' + key)
    require(result['context_id'] == digest(context), 'Context digest')
    options = context['options']
    require(context['action'] == 'history' and options['history_scope'] == 'private-draw', 'Explicit scope')
    require(data['frame_sha256'] == context['frame_sha256'] and data['event'] == options['gpa_event'], 'Data context')
    for key in ('vendor', 'device', 'luid_low', 'luid_high', 'warp'):
        require(data['adapter'][key] == context['adapter'][key], 'Actual adapter join')
    for key in ('resource', 'x', 'y', 'mip', 'layer', 'slice', 'sample'):
        require(data['selection'][key] == options.get(key, 0), 'Selection join: ' + key)
    outputs = options.get('shader_outputs', False) or options.get('output_merger', False)
    require(data['shader_evaluation']['requested'] is outputs and
            data['merger_evaluation']['requested'] is options.get('output_merger', False) and
            data['primitive_identity_observation']['requested'] is options.get('primitive_identity', False),
            'Requested diagnostic options')
    require(data.get('primitive_presence_observation', {}).get('requested', False) is options.get('primitive_presence', False),
            'Requested primitive presence')
    storage(data, vendor)
    if outputs:
        shader(data, vendor)
    if options.get('output_merger', False):
        merger(data, vendor)
    if options.get('primitive_identity', False):
        validate_history_identity(data)
    execution = result['execution']
    require(execution['elapsed_ms'] >= 0 and execution['peak_working_set_bytes'] > 0 and
            len(execution['loaded_modules']) > 0, 'Execution telemetry')
    for module in execution['loaded_modules']:
        path = module.lower().replace('\\', '/')
        require('renderdoc' not in path and '/intelswtools/gpa/' not in path and
                path.rsplit('/', 1)[-1] not in ('dx11_player.dll', 'dx11_playback.dll'), 'Forbidden loaded module')
    require(set(result['artifacts']) == {'history.json'}, 'Export contract')
    return data


def read(directory):
    job = json.loads((directory / 'job.json').read_text(encoding='utf-8'))
    result = json.loads((directory / 'result.json').read_text(encoding='utf-8'))
    payload = (directory / 'history.json').read_bytes()
    manifest = result['artifacts']['history.json']
    require(manifest['bytes'] == len(payload) and manifest['sha256'] == hashlib.sha256(payload).hexdigest() and
            json.loads(payload) == result['data'], 'Saved history integrity')
    return job, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--real', type=Path)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--with-presence', action='store_true', help='Require the extended six-mode owned matrix')
    args = parser.parse_args()
    modes = ('storage', 'shader', 'merger', 'identity', 'all-msaa')
    if args.with_presence:
        modes += ('presence',)
    expected = {f'{vendor}-{mode}' for vendor in (0, 4318, 32902) for mode in modes}
    paths = sorted(p for p in args.directory.iterdir() if p.is_dir())
    require({p.name for p in paths} == expected, 'Complete owned job matrix')
    summaries, controls, real_count = [], 0, 0
    for directory in paths:
        vendor_text, mode = directory.name.split('-', 1)
        vendor = int(vendor_text)
        job, result = read(directory)
        data = validate(job, result, vendor)
        require(data['before']['bytes'] == list(struct.pack('<4f', .125, .5, .25, .25)) and
                data['after']['bytes'] == list(struct.pack('<4f', .25, .75, .125, .5)), 'Authored shader/storage oracle')
        require(len(data['records']) == 3 and data['selection']['sample'] == (2 if mode == 'all-msaa' else 0),
                'Authored primitive/sample selection')
        require(data['shader_evaluation']['requested'] is (mode in ('shader', 'merger', 'all-msaa')) and
                data['merger_evaluation']['requested'] is (mode in ('merger', 'all-msaa')) and
                data['primitive_identity_observation']['requested'] is (mode in ('identity', 'all-msaa')),
                'Owned mode dispatch')
        if mode in ('identity', 'all-msaa'):
            require(data['primitive_identity_observation']['system_primitive_id'] == 2 and
                    data['primitive_identity_association']['matching_primitive_ordinals'] == [2], 'Authored last primitive')
        mutations = [lambda r: r.update(context_id='changed'),
                     lambda r: r['data']['selection'].update(sample=7),
                     lambda r: r['data']['after']['bytes'].__setitem__(0, 255),
                     lambda r: r['data'].update(original_fragment_eligibility=True),
                     lambda r: r['data']['shader_evaluation'].update(requested=not data['shader_evaluation']['requested']),
                     lambda r: r['execution']['loaded_modules'].append('C:/unexpected/renderdoc.dll')]
        for mutate in mutations:
            changed = copy.deepcopy(result)
            mutate(changed)
            try:
                validate(job, changed, vendor)
            except (AssertionError, KeyError, ValueError):
                controls += 1
            else:
                raise AssertionError('Corrupted job result accepted')
        summaries.append({'case': directory.name, 'context_id': result['context_id'],
                          'elapsed_ms': result['execution']['elapsed_ms'],
                          'peak_working_set_bytes': result['execution']['peak_working_set_bytes']})
    if args.real:
        require(args.reference is not None, 'Real jobs require independent prior probe evidence')
        expected_real = {f'{name}-{vendor}' for name in ('gf2', 'bf1', 'helldivers') for vendor in (0, 4318, 32902)}
        real_paths = sorted(p for p in args.real.iterdir() if p.is_dir())
        require({p.name for p in real_paths} == expected_real, 'Complete real job matrix')
        for directory in real_paths:
            vendor = int(directory.name.rsplit('-', 1)[1])
            job, result = read(directory)
            data = validate(job, result, vendor)
            reference = json.loads((args.reference / directory.name / 'report.json').read_text(encoding='utf-8'))
            require(reference['probe']['status'] == 'ok', 'Independent ordinary replay comparison')
            for key in ('frame_sha256', 'event', 'selection', 'adapter', 'before', 'after', 'restored'):
                require(data[key] == reference[key], 'Real probe agreement: ' + key)
            require(data['primitive_identity_observation']['system_primitive_id'] ==
                    reference['primitive_identity_observation']['system_primitive_id'], 'Real system ID')
            real_count += 1
            summaries.append({'case': directory.name, 'context_id': result['context_id'],
                              'elapsed_ms': result['execution']['elapsed_ms'],
                              'peak_working_set_bytes': result['execution']['peak_working_set_bytes']})
    with args.out.open('x', encoding='utf-8') as output:
        json.dump({'schema': 1, 'status': 'ok', 'owned_jobs': len(paths), 'real_jobs': real_count,
                   'negative_controls': controls, 'original_fragment_history_complete': False,
                   'jobs': summaries}, output, indent=2)
        output.write('\n')
    print(json.dumps({'owned_jobs': len(paths), 'real_jobs': real_count, 'negative_controls': controls}))


if __name__ == '__main__':
    main()
