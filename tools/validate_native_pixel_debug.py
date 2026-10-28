"""Validate explicit private-primitive PS jobs, input joins and reversible journals.

Original draw fragment eligibility and complete PS instruction parity are not
established by this validator. The C++ reader reports are independently replayed
in Python; owned shader semantics are checked by NativePixelDebugTests.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path

from validate_native_draw_history_jobs import digest, require
from validate_native_pixel_history_shader import validate as history
from validate_dxbc_execution_trace import validate as journal


def validate(job, result, vendor, reader):
    context, data = result['context'], result['data']
    require(result['schema'] == job['schema'] == 1 and result['ok'] is True and
            result['request_id'] == job['request_id'] and result['backend'] == job['backend'], 'Job identity')
    for key in ('schema', 'backend', 'backend_sha256', 'frame_sha256', 'experiment_sha256', 'action', 'options'):
        require(context[key] == job[key], 'Context: ' + key)
    require(result['context_id'] == digest(context), 'Context digest')
    options = context['options']
    require(context['action'] == 'debug-pixel' and options['debug_scope'] == 'private-primitive', 'Explicit scope')
    require(data['format'] == 'flora.native-pixel-debug' and data['version'] == 1 and
            data['trace_format'] == 'flora.dxbc-execution-trace/1' and
            data['captured_gpu_schedule'] is False, 'Debug schema')
    require(data['trace_file'] == 'trace.jsonl' and data['shader_file'] == 'shader.dxbc' and
            set(result['artifacts']) == {'debug.json', 'trace.jsonl', 'shader.dxbc'}, 'Artifacts')
    require(data['shader_sha256'] == result['artifacts']['shader.dxbc']['sha256'], 'Shader digest')
    inv, private = data['invocation'], data['private_history']
    require(inv['status'] == 'explicit_private_primitive' and inv['original_fragment_eligibility'] is None and
            inv['instance'] is None and inv['gpa_event'] == options['gpa_event'] and
            inv['gpa_shader'] == data['gpa_shader'] and inv['primitive_ordinal'] == options['primitive'] and
            inv['resource_boundary'] == 'before_private_primitive', 'Invocation scope and identity')
    history(private, vendor)
    require(private['frame_sha256'] == data['effective_frame_sha256'] and private['event'] == inv['gpa_event'] and
            private['selection'] == inv['selection'], 'History context')
    for key in ('vendor', 'device', 'luid_low', 'luid_high', 'warp'):
        require(private['adapter'][key] == context['adapter'][key], 'Actual adapter')
    for key in ('resource', 'x', 'y', 'mip', 'layer', 'slice', 'sample'):
        require(inv['selection'][key] == options.get(key, 0), 'Selection: ' + key)
    provenance = inv['quad_input_provenance']
    require(provenance['resource_boundary'] == 'before_private_primitive' and
            provenance['completed_primitives'] == inv['primitive_ordinal'] == provenance['primitive_ordinal'] and
            provenance['original_primitive_id'] == inv['original_primitive_id'] and
            provenance['original_fragment_eligibility'] is None, 'Quad boundary')
    measured = {key: value for key, value in provenance.items()
                if key not in ('resource_boundary', 'completed_primitives')}
    matching = []
    for row in private['records']:
        if row['primitive_ordinal'] == inv['primitive_ordinal']:
            require(row['original_primitive_id'] == inv['original_primitive_id'], 'Candidate identity')
            matching += [output for output in row['private_shader_outputs']
                         if output['candidate_raster_samples'] == inv['candidate_raster_samples']]
    require(len(matching) == 1, 'Unique measured invocation')
    output = matching[0]
    require(output['input_provenance'] == measured and output['selected_lane'] == inv['selected_lane'] and
            output['initial_active_mask'] == inv['active_mask'] and
            output['shader_sha256'] == data['shader_sha256'] and output['shader'] == data['gpa_shader'] and
            options.get('sample', 0) in inv['candidate_raster_samples'], 'Measured input join')
    header = reader['header']
    require(header['stage'] == 'ps' and header['gpa_event'] == inv['gpa_event'] and
            header['gpa_shader'] == data['gpa_shader'] and header['selected_lane'] == inv['selected_lane'] and
            header['initial_active_mask'] == inv['active_mask'] and header['quad_input_provenance'] == provenance and
            header['execution_model'] == data['execution_model'] and
            header['resource_observation'] == 'current_native_bindings', 'Trace selection')
    adapter = context['adapter']
    require(header['adapter'] == dict(vendor=adapter['vendor'], device=adapter['device'],
                                     luid=(adapter['luid_high'] << 32) | adapter['luid_low']), 'Trace adapter')
    execution = result['execution']
    require(execution['elapsed_ms'] >= 0 and execution['peak_working_set_bytes'] > 0 and
            bool(execution['loaded_modules']), 'Telemetry')
    for module in execution['loaded_modules']:
        path = module.lower().replace('\\', '/')
        require('renderdoc' not in path and '/intelswtools/gpa/' not in path and
                path.rsplit('/', 1)[-1] not in ('dx11_player.dll', 'dx11_playback.dll'), 'Forbidden module')


def read(directory):
    job = json.loads((directory / 'job.json').read_text(encoding='utf-8'))
    result = json.loads((directory / 'result.json').read_text(encoding='utf-8'))
    for name, entry in result['artifacts'].items():
        require(name in ('debug.json', 'shader.dxbc', 'trace.jsonl'), 'Unexpected artifact path')
        raw = (directory / name).read_bytes()
        require(len(raw) == entry['bytes'] and hashlib.sha256(raw).hexdigest() == entry['sha256'], 'Artifact digest')
    require(json.loads((directory / 'debug.json').read_text(encoding='utf-8')) == result['data'], 'Saved metadata')
    return job, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--real', type=Path)
    parser.add_argument('--readers', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    owned = sorted(p for p in args.directory.iterdir() if p.is_dir())
    require({p.name for p in owned} == {f'{vendor}-{mode}' for vendor in (0, 4318, 32902)
                                      for mode in ('constant', 'derivative', 'sample', 'discard', 'source', 'uav')},
            'Complete owned matrix')
    real = sorted(p for p in args.real.iterdir() if p.is_dir()) if args.real else []
    if args.real:
        require({p.name for p in real} == {f'{name}-{vendor}' for name in ('gf2', 'bf1', 'helldivers')
                                         for vendor in (0, 4318, 32902)}, 'Complete real matrix')
    summaries, controls = [], 0
    for directory in owned + real:
        vendor = int(directory.name.split('-')[0] if directory in owned else directory.name.rsplit('-', 1)[1])
        job, result = read(directory)
        reader = json.loads((args.readers / (directory.name + '.reader.json')).read_text(encoding='utf-8'))
        validate(job, result, vendor, reader)
        data = result['data']
        evidence = dict(frame_sha256=data['effective_frame_sha256'], shader_sha256=data['shader_sha256'],
                        event=data['invocation']['gpa_event'], shader=data['gpa_shader'],
                        adapter_luid=reader['header']['adapter']['luid'], trace=copy.deepcopy(data['trace']))
        for space, key in (('buffer', 'buffers'), ('texture', 'textures')):
            evidence[key] = [v for v in reader['initial']['storage'] if v['space'] == space]
            evidence['trace'][key + '_after'] = [v for v in reader['final']['storage'] if v['space'] == space]
        trace = journal(directory / 'trace.jsonl', evidence, reader)
        mutations = [lambda r: r.update(context_id='changed'),
                     lambda r: r['data']['invocation'].update(original_fragment_eligibility=True),
                     lambda r: r['data']['invocation'].update(primitive_ordinal=9999),
                     lambda r: r['data']['invocation']['selection'].update(sample=31),
                     lambda r: r['data']['invocation'].update(selected_lane=9),
                     lambda r: r['data']['invocation']['quad_input_provenance'].update(completed_primitives=9999),
                     lambda r: r['execution']['loaded_modules'].append('C:/unexpected/renderdoc.dll')]
        for mutate in mutations:
            changed = copy.deepcopy(result)
            mutate(changed)
            try:
                validate(job, changed, vendor, reader)
            except (AssertionError, ValueError, KeyError, IndexError):
                controls += 1
            else:
                raise AssertionError('Corrupt private PS result accepted')
        summaries.append(dict(case=directory.name, context_id=result['context_id'], trace=trace,
                              elapsed_ms=result['execution']['elapsed_ms'],
                              peak_working_set_bytes=result['execution']['peak_working_set_bytes']))
    report = dict(schema=1, status='ok', owned_jobs=len(owned), real_jobs=len(real), negative_controls=controls,
                  original_fragment_debugging_complete=False, jobs=summaries)
    with args.out.open('x', encoding='utf-8') as output:
        json.dump(report, output, indent=2)
        output.write('\n')
    print(json.dumps({key: report[key] for key in ('status', 'owned_jobs', 'real_jobs', 'negative_controls')}))


if __name__ == '__main__':
    main()
