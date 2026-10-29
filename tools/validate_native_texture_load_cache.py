"""Validate paired captured texture-load observations and original GPU controls.

The selected single-group dispatch is a bounded regression/performance sample;
it does not establish full debugger or release parity.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path

from validate_native_compute_captures import comparison
from validate_native_compute_debug import validate_job, negative_checks
from validate_native_debug_session import require, validate as validate_archive


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pair(before, after, vendor):
    controls = [comparison(item, vendor) for item in (before, after)]
    for key in ('frame_sha256', 'shader_sha256', 'stage', 'event', 'shader', 'adapter_vendor',
                'adapter_device', 'adapter_luid', 'buffers', 'textures', 'texture_views',
                'texture_metadata', 'bindings', 'samplers', 'counters', 'sample_queries',
                'multisample_inputs'):
        require(before[key] == after[key], 'Paired input identity: ' + key)
    old, new = before['execution'], after['execution']
    require(old['helper']['backend'] == 'dxbc-gpu/27' and new['helper']['backend'] == 'dxbc-gpu/28',
            'Executor version')
    for key in old['helper']:
        if key != 'backend':
            require(old['helper'][key] == new['helper'][key], 'Paired GPU helper: ' + key)
    for key in ('buffers_after', 'textures_after', 'last_completed_step', 'steps', 'texture_writes',
                'texture_write_bytes', 'group', 'model', 'step_limit', 'timeout_ms'):
        require(old[key] == new[key], 'Paired execution: ' + key)
    require(old['gpu_operations'] == 3072 and new['gpu_operations'] == 2688,
            'Physical GPU work reduction')
    require(old['texture_writes'] == 512 and old['texture_write_bytes'] == 4096,
            'Texture write coverage')
    return dict(vendor=vendor, controls=controls, before_operations=old['gpu_operations'],
                after_operations=new['gpu_operations'], before_ms=old['elapsed_ms'],
                after_ms=new['elapsed_ms'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--evidence', action='append', type=Path, default=[])
    parser.add_argument('--jobs', type=Path)
    parser.add_argument('--sessions', type=Path)
    parser.add_argument('--worker', type=Path)
    parser.add_argument('--reload-probe', type=Path)
    parser.add_argument('--baseline-trace', type=Path)
    args = parser.parse_args()
    workflow_args = (args.jobs, args.sessions, args.worker, args.reload_probe, args.baseline_trace)
    require(all(workflow_args) or not any(workflow_args), 'Supply all workflow arguments together')
    matrix = read(args.directory / 'matrix.json')
    require(matrix['schema'] == 1 and matrix['full_release_acceptance'] is False, 'Matrix schema/scope')
    require(matrix['inputs']['frame']['sha256'] ==
            '26044d33cb6c3087c2204b7d57664203741a679f988430b7d05b24af582e0620', 'Captured frame identity')
    for row in matrix['inputs'].values():
        require(digest(Path(row['path'])) == row['sha256'], 'Input artifact changed')
    expected = [(repeat, vendor, version) for repeat in range(2) for vendor in (0, 4318, 32902)
                for version in (('before', 'after') if repeat == 0 else ('after', 'before'))]
    require([(r['repeat'], r['vendor'], r['version']) for r in matrix['cases']] == expected,
            'Serial matrix coverage/order')
    reports = {}
    for row in matrix['cases']:
        require(row['exit_code'] == 0 and not row['timed_out'], 'Probe process failed')
        label = f"{row['repeat']}-{row['vendor']}-{row['version']}"
        require(row['label'] == label, 'Probe label')
        path = args.directory / (label + '.json')
        require(digest(path) == row['report_sha256'], 'Raw report changed')
        data = read(path)
        require(data['execution'] == row['execution'] and
                data['frame_sha256'] == matrix['inputs']['frame']['sha256'], 'Raw execution identity')
        command = row['command']
        require(command[0] == matrix['inputs'][row['version']]['path'] and
                command[1] == matrix['inputs']['frame']['path'] and
                command[2:5] == ['325', '5', str(row['vendor'])] and
                Path(command[5]).resolve() == path.resolve() and
                command[6:] == ['--execute-group', '--compare-native'], 'Probe invocation')
        reports[(row['repeat'], row['vendor'], row['version'])] = data
    pairs, rejected = [], 0
    for repeat in range(2):
        for vendor in (0, 4318, 32902):
            before, after = [reports[(repeat, vendor, version)] for version in ('before', 'after')]
            pairs.append(dict(repeat=repeat, **pair(before, after, vendor)))
            # Reject stale input, changed output, loss of GPU savings and false control success.
            for mutation in range(4):
                changed = copy.deepcopy(after)
                if mutation == 0:
                    changed['textures'][0]['sha256'] = '0' * 64
                elif mutation == 1:
                    changed['execution']['textures_after'][0]['sha256'] = '0' * 64
                elif mutation == 2:
                    changed['execution']['gpu_operations'] = 3072
                else:
                    changed['execution']['native_comparison']['matching_native_inputs'] = False
                try:
                    pair(before, changed, vendor)
                except ValueError:
                    rejected += 1
                else:
                    raise ValueError('Corrupted comparison was accepted')
    workflow = None
    files = list(args.directory.rglob('*')) + args.evidence
    if args.jobs:
        jobs, sessions = read(args.jobs / 'matrix.json'), read(args.sessions / 'matrix.json')
        require(len(jobs['cases']) == 1 and len(sessions['cases']) == 2 and
                all(r['exit_code'] == 0 and not r['timed_out'] for r in jobs['cases'] + sessions['cases']),
                'CLI/session process matrix')
        require(jobs['binaries'][1]['sha256'] == digest(args.worker), 'Workflow Worker identity')
        directory = args.jobs / '0'
        result = read(directory / 'result.json')
        reference = copy.deepcopy(reports[(0, 4318, 'after')])
        reference['trace'] = dict(result['data']['trace'],
                                  buffers_after=reference['execution']['buffers_after'],
                                  textures_after=reference['execution']['textures_after'])
        journal = validate_job(directory, reference, [1, 1, 1], args.worker,
                               read(args.sessions / '0.reader.json'))
        negative = negative_checks(directory, reference, [1, 1, 1], args.worker)
        archive = validate_archive(args.sessions / '0.zip', directory, args.reload_probe)
        before = args.baseline_trace.read_bytes().splitlines()
        after = (directory / 'trace.jsonl').read_bytes().splitlines()
        require(len(before) == len(after) == 24111 and before[:-1] == after[:-1],
                'Instruction journal changed')
        old, new = json.loads(before[-1]), json.loads(after[-1])
        require(old['record'] == new['record'] == 'complete' and
                old['gpu']['backend'] == 'dxbc-gpu/27' and new['gpu']['backend'] == 'dxbc-gpu/28' and
                old['gpu_operations'] == 3072 and new['gpu_operations'] == 2688, 'Journal completion identity')
        del old['gpu']['backend'], new['gpu']['backend'], old['gpu_operations'], new['gpu_operations']
        require(old == new, 'Journal completion semantics changed')
        workflow = dict(journal=journal, negative=negative, archive=archive,
                        identical_nonterminal_records=24110, baseline_trace_sha256=digest(args.baseline_trace))
        files += list(args.jobs.rglob('*')) + list(args.sessions.rglob('*')) + [args.baseline_trace]
    evidence = [dict(path=path.as_posix(), bytes=path.stat().st_size, sha256=digest(path))
                for path in sorted(set(files)) if path.is_file() and path.resolve() != args.out.resolve()]
    result = dict(schema=1, passed=True, full_release_acceptance=False, pairs=pairs,
                  negative_checks=rejected, workflow=workflow, evidence=evidence)
    args.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(passed=True, pairs=len(pairs), negative_checks=rejected, evidence=len(evidence))))


if __name__ == '__main__':
    main()
