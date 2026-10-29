"""Check captured compute transport, reversible journals and the separate BF1 GPU controls.

Only event 325 has a whole-dispatch arithmetic comparison. Other jobs establish
captured invocation selection and journal/session continuity, not output parity.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path

from validate_dxbc_execution_trace import validate as validate_journal
from validate_native_compute_debug import validate_job, negative_checks
from validate_native_debug_session import validate as validate_archive, require


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def metadata(job, result, expected, reader):
    event, groups = expected
    require(result['schema'] == job['schema'] == 1 and result['backend'] == job['backend'] == 'dx11-native/11' and
            result['ok'] and result['request_id'] == job['request_id'], 'Job completion identity')
    context = result['context']
    for key in ('schema', 'backend', 'backend_sha256', 'frame_sha256', 'experiment_sha256', 'action', 'options'):
        require(context[key] == job[key], 'Stale context: ' + key)
    raw = json.dumps(context, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()
    require(hashlib.sha256(raw).hexdigest() == result['context_id'], 'Context digest')
    options = context['options']
    require(context['action'] == 'debug-thread' and options['gpa_event'] == event and
            options['gpu_vendor'] == 4318 and options['group'] == options['thread'] == [0, 0, 0] and
            context['experiment_sha256'] is None, 'Selected captured invocation')
    adapter = context['adapter']
    require(adapter['vendor'] == 4318 and not adapter['warp'], 'Selected physical adapter')
    data = result['data']
    invocation = data['invocation']
    require(data['format'] == 'flora.native-compute-debug' and data['version'] == 1 and
            data['effective_frame_sha256'] == context['frame_sha256'] and data['captured_gpu_schedule'] is False and
            invocation['gpa_event'] == event and invocation['dispatch_groups'] == groups and
            invocation['status'] == 'eligible_in_observed_replay' and invocation['dispatch_kind'] == 'direct' and
            invocation['group'] == invocation['thread'] == [0, 0, 0], 'Invocation provenance')
    header = reader['header']
    require(header['stage'] == 'cs' and header['gpa_event'] == event and
            header['gpa_shader'] == data['gpa_shader'] and header['shader_sha256'] == data['shader_sha256'] and
            header['group_size'] == invocation['group_size'] and
            header['adapter']['luid'] == (adapter['luid_high'] << 32) | adapter['luid_low'], 'Reader selection')
    require(reader['round_trip'] and reader['debug_model']['register_round_trip'] and
            reader['debug_model']['configuration_round_trip'], 'Reader/model restoration')
    require(result['execution']['loaded_modules'] and result['execution']['peak_working_set_bytes'] > 0,
            'Execution telemetry missing')
    for name in result['execution']['loaded_modules']:
        normalized = name.lower().replace('\\', '/')
        require('renderdoc' not in normalized and '/intelswtools/gpa/' not in normalized and
                normalized.rsplit('/', 1)[-1] not in ('dx11_player.dll', 'dx11_playback.dll'),
                'External analysis module')


def comparison(reference, vendor):
    execution = reference['execution']
    actual = execution['native_comparison']
    require(reference['observed'] and reference['event'] == 325 and reference['shader'] == 318 and
            reference['adapter_vendor'] == (vendor or 5140) and reference['requested_vendor'] == vendor and
            not reference['unavailable'], 'Independent GPU input identity')
    require(execution['completed'] and execution['steps'] == 22272 and
            execution['dispatch_arguments'] == [1, 1, 1] and execution['thread_group_size'] == [16, 16, 1] and
            execution['native_output_parity_verified'] and actual['attempted'] and actual['passed'] and
            actual['matching_native_inputs'] and actual['original_observed_resources_unchanged'],
            'Incomplete original GPU comparison')
    require({r['resource'] for r in actual['resources']} == {35, 193, 273}, 'Comparison resource coverage')
    outputs = {r['resource']: r['sha256'] for r in execution['buffers_after'] + execution['textures_after']}
    for row in actual['resources']:
        require(row['equal'] and row['expected_sha256'] == row['native_sha256'] == outputs[row['resource']],
                'Original GPU output mismatch')
    return dict(vendor=vendor, adapter_luid=reference['adapter_luid'], resources=actual['resources'],
                execution_ms=execution['elapsed_ms'], comparison_ms=actual['elapsed_ms'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('jobs', type=Path)
    parser.add_argument('sessions', type=Path)
    parser.add_argument('--worker', required=True, type=Path)
    parser.add_argument('--reload-probe', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--evidence', action='append', type=Path, default=[])
    args = parser.parse_args()
    matrix = read(args.jobs / 'matrix.json')
    session_matrix = read(args.sessions / 'matrix.json')
    expected = [(279, [2, 1, 1]), (325, [1, 1, 1]), (370, [2, 1, 1]),
                (904, [32, 32, 1]), (2595, [32, 32, 1]), (2699, [4, 4, 1])]
    require(len(matrix['cases']) == 6 and len(session_matrix['cases']) == 12 and
            all(r['exit_code'] == 0 and not r['timed_out'] for r in session_matrix['cases']),
            'Incomplete job/session matrix')
    require(matrix['binaries'][1]['sha256'] == digest(args.worker), 'Worker binary identity')
    comparisons, rejected = [], 0
    for vendor, suffix in ((4318, 'reference'), (0, 'warp-reference'), (32902, 'intel-reference')):
        reference = read(args.jobs / f'bf1-325-{suffix}.json')
        comparisons.append(comparison(reference, vendor))
        changed = copy.deepcopy(reference)
        changed['execution']['native_comparison']['resources'][0]['native_sha256'] = '0' * 64
        try:
            comparison(changed, vendor)
        except ValueError:
            rejected += 1
        else:
            raise ValueError('Accepted corrupted GPU comparison')
    summaries = []
    for index, row in enumerate(matrix['cases']):
        require(row['index'] == index and row['event'] == expected[index][0] and
                row['exit_code'] == 0 and not row['timed_out'], 'Job matrix row')
        directory = args.jobs / str(index)
        job, result = read(directory / 'job.json'), read(directory / 'result.json')
        require(result == row['result'] and job['frame_sha256'] == row['frame_sha256'], 'Retained job identity')
        reader = read(args.sessions / f'{index}.reader.json')
        metadata(job, result, expected[index], reader)
        data = result['data']
        evidence = dict(frame_sha256=row['frame_sha256'], event=row['event'], shader=data['gpa_shader'],
                        shader_sha256=data['shader_sha256'], adapter_luid=reader['header']['adapter']['luid'],
                        trace=copy.deepcopy(data['trace']))
        for space, key in (('buffer', 'buffers'), ('texture', 'textures')):
            evidence[key] = [r for r in reader['initial']['storage'] if r['space'] == space]
            evidence['trace'][key + '_after'] = [r for r in reader['final']['storage'] if r['space'] == space]
        journal = validate_journal(directory / 'trace.jsonl', evidence, reader)
        archive = validate_archive(args.sessions / f'{index}.zip', directory, args.reload_probe)
        for mode in range(3):
            changed = copy.deepcopy(result)
            if mode == 0:
                changed['data']['invocation']['dispatch_groups'][0] += 1
            elif mode == 1:
                changed['data']['captured_gpu_schedule'] = True
            else:
                changed['execution']['loaded_modules'].append('C:/unexpected/renderdoc.dll')
            try:
                metadata(job, changed, expected[index], reader)
            except ValueError:
                rejected += 1
            else:
                raise ValueError('Accepted corrupted job metadata')
        if index == 1:
            reference = read(args.jobs / 'bf1-325-reference.json')
            reference['trace'] = dict(data['trace'], buffers_after=reference['execution']['buffers_after'],
                                      textures_after=reference['execution']['textures_after'])
            validate_job(directory, reference, [1, 1, 1], args.worker, reader)
            rejected += len(negative_checks(directory, reference, [1, 1, 1], args.worker)['cases'])
        summaries.append(dict(event=row['event'], shader=data['gpa_shader'], journal=journal,
                              archive=archive, execution=result['execution']))
        print(json.dumps(dict(event=row['event'], validated=True, reloaded=archive['reload']['resumed'])), flush=True)
    files = [p for root in (args.jobs, args.sessions) for p in sorted(root.rglob('*')) if p.is_file()]
    files += [Path(__file__), args.worker, args.reload_probe] + args.evidence
    files += [Path('tools') / name for name in ('probe_native_compute_captures.py',
              'probe_native_compute_sessions.py', 'validate_native_compute_debug.py',
              'validate_dxbc_execution_trace.py', 'validate_native_debug_session.py')]
    result = dict(schema=1, passed=True, full_parity=False, jobs=summaries, comparisons=comparisons,
                  rejected_corruptions=rejected,
                  files=[dict(path=p.resolve().as_posix(), sha256=digest(p), bytes=p.stat().st_size) for p in files])
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps(dict(passed=True, jobs=len(summaries), comparisons=len(comparisons), rejected=rejected)))


if __name__ == '__main__':
    main()
