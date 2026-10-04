"""Run registered corpora serially and verify positives and located negative outcomes.

Development only. Expected rejection never means any nonzero process exit is OK.
No GPU/original-player equivalence is inferred from this gate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def manifest_digest(path):
    # Git requires LF; working-tree generators may have emitted CRLF on Windows.
    # Pin all other bytes, while keeping raw process-input hashes in run reports.
    return hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def check_case(case, specification, negative, directory, repeat):
    """Check actual reports and logs, including count/completion/dependency evidence."""
    preflight = case.get('preflight', {})
    report = preflight.get('report') or {}
    require(not preflight.get('timed_out') and report.get('completed') is True and
            not report.get('cancelled'), 'Incomplete preflight')
    require(report.get('gpu_validation') == 'not_run' and report.get('replay_proven') is False,
            'Preflight incorrectly claims GPU validation')
    errors = [f for f in report.get('findings', []) if f['severity'] == 'error']
    runs = case.get('native', [])
    require(len(runs) == repeat, 'Missing native attempts')
    require(all(not r.get('timed_out') and not r.get('launch_error') for r in runs),
            'Native timeout or launch failure')
    require(report.get('errors') == len(errors), 'Inconsistent diagnostic count')
    if negative:
        actual = [{k: f.get(k) for k in ['kind', 'event_id', 'resource_id', 'record_type']} for f in errors]
        require(actual == negative['findings'], 'Rejection diagnostics/location changed')
        require(preflight.get('exit_code') == 2 and report.get('status') == 'blocked',
                'Negative preflight did not reject')
        require(case.get('status') == 'replay_failed', 'Expected rejection became replayable')
        for index, run in enumerate(runs, 1):
            require(run.get('exit_code') == 1 and not run.get('report', {}).get('completed'),
                    'Unexpected negative exit or completed replay')
            # Progress text can precede the machine-readable failure line.
            text = (directory / f'native-{index}.log').read_text(encoding='utf-8')
            failures = []
            for line in text.splitlines():
                try:
                    value = json.loads(line)
                except ValueError:
                    continue
                if isinstance(value, dict) and value.get('completed') is False:
                    failures.append(value)
            require(len(failures) == 1 and failures[0].get('error') == negative['runtime_error'],
                    'Runtime rejection reason changed or missing')
    else:
        require(preflight.get('exit_code') == 0 and not errors and
                report.get('status') in {'checked', 'review_required'}, 'Positive preflight blocked')
        allowed = {'repeat_stable'}
        if specification['comparison_policy'] == 'known_variable':
            allowed.add('repeat_variable')
        require(case.get('status') in allowed, 'Unexpected replay or determinism outcome')
        for run in runs:
            native = run.get('report', {})
            require(run.get('exit_code') == 0 and native.get('completed') is True and
                    run.get('runtime_dependency_audit') == 'passed', 'Replay/dependency check failed')
            expected = specification.get('reference_rgba_sha256')
            require(not expected or native.get('rgba_sha256') == expected, 'Golden pixels changed')
            require(native.get('rgba_sha256') and
                    run.get('artifacts', {}).get('frame.rgba') == native['rgba_sha256'],
                    'Missing or inconsistent raw image evidence')
        hashes = [r['report']['rgba_sha256'] for r in runs]
        require(specification['comparison_policy'] == 'known_variable' or len(set(hashes)) == 1,
                'Deterministic images vary despite reported status')
    controls = case.get('controls', [])
    require(len(controls) == len(specification.get('controls', [])), 'Missing controls')
    for actual, expected in zip(controls, specification.get('controls', [])):
        require(actual.get('passed') is True and actual.get('disabled_event') == expected['disable_event']
                and len(actual.get('runs', [])) == expected['repeat'], 'Failed or incomplete control')
        require(all(not r.get('timed_out') and r.get('exit_code') == 0 and
                    r.get('report', {}).get('completed') is True and
                    r['report'].get('rgba_sha256') == expected['expected_rgba_sha256']
                    for r in actual['runs']), 'Control evidence disagrees')
    boundaries = case.get('boundaries', [])
    require(len(boundaries) == len(specification.get('boundaries', [])), 'Missing resource boundaries')
    for actual, expected in zip(boundaries, specification.get('boundaries', [])):
        require(actual.get('passed') is True and actual.get('complete') is True and
                actual.get('event') == expected['event'] and actual.get('resource') == expected['resource']
                and len(actual.get('runs', [])) == repeat, 'Failed or incomplete resource boundary')
        require(all(not r.get('timed_out') and r.get('exit_code') == 0 and
                    r.get('report', {}).get('completed') is True for r in actual['runs']),
                'Boundary execution failed')
        hashes = actual.get('storage_sha256', [])
        require(len(hashes) == repeat and all(hashes), 'Missing resource bytes')
        require(not expected['expect_stable'] or len(set(hashes)) == 1, 'Resource bytes vary')
        golden = expected.get('expected_storage_sha256')
        require(not golden or all(h == golden for h in hashes), 'Resource byte golden changed')


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--legacy-root', type=Path, required=True)
    parser.add_argument('--artifacts-root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--suites', type=Path, default=Path('docs/m2-gate-suites.json'))
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    config = load(args.suites)
    require(config.get('manifest_hash_policy') == 'UTF-8 bytes with CRLF normalized to LF',
            'Unknown manifest hash policy')
    exe = args.exe.resolve(strict=True)
    roots = {'legacy': args.legacy_root.resolve(strict=True),
             'artifacts': args.artifacts_root.resolve(strict=True)}
    args.out.mkdir(parents=True, exist_ok=False)
    summary = dict(schema='FloraGPA compatibility gate 1', completed=False, passed=False,
                   exe_sha256=digest(exe), suites_sha256=digest(args.suites),
                   repeat=2, suites=[], cases=0, positive_cases=0, rejected_cases=0,
                   unique_capture_sha256={}, limits=[
                       'Registered corpus gate, not complete API/version coverage',
                       'No original-player comparison is newly performed by this runner',
                       'Metadata decoding is not execution; byte equality does not certify all states'])

    def save():
        (args.out / 'gate.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')

    save()
    try:
        for suite in config['suites']:
            name = suite['id']
            require(Path(name).name == name and name not in {'.', '..'}, 'Invalid suite ID')
            manifest = repo / suite['manifest']
            require(manifest_digest(manifest) == suite['manifest_sha256'], 'Changed corpus manifest: ' + name)
            specifications = load(manifest)['cases']
            require(set(suite['expected_rejections']) <= {c['id'] for c in specifications},
                    'Unknown expected rejection')
            output = args.out / name
            command = [sys.executable, str(repo / 'tools/validate_corpus.py'), '--exe', str(exe),
                       '--manifest', str(manifest), '--captures-root',
                       str(roots[suite['capture_root']] / suite['subdirectory']),
                       '--out', str(output), '--repeat', '2', '--timeout', '90']
            # One child at a time. validate_corpus enforces individual process timeouts.
            with (args.out / (name + '.log')).open('wb') as log:
                code = subprocess.call(command, stdout=log, stderr=subprocess.STDOUT)
            result = load(output / 'validation.json')
            require(code == int(bool(suite['expected_rejections'])), 'Unexpected suite exit: ' + name)
            require(result['exe_sha256'] == summary['exe_sha256'] and
                    result['manifest_sha256'] == digest(manifest), 'Evidence identity mismatch')
            require([c['id'] for c in result['cases']] == [c['id'] for c in specifications],
                    'Missing/reordered corpus cases')
            for case, spec in zip(result['cases'], specifications):
                check_case(case, spec, suite['expected_rejections'].get(case['id']),
                           output / case['id'], 2)
                summary['cases'] += 1
                summary['rejected_cases' if case['id'] in suite['expected_rejections']
                        else 'positive_cases'] += 1
                summary['unique_capture_sha256'].setdefault(spec['sha256'], []).append(name+'/'+case['id'])
            summary['suites'].append(dict(id=name, passed=True, report_sha256=digest(output/'validation.json'),
                                         cases=len(specifications), rejections=len(suite['expected_rejections'])))
            save()
            print(name, 'passed', len(specifications), 'cases', flush=True)
        require(summary['cases'] == config['expected_cases'] and
                summary['rejected_cases'] == config['expected_rejections'] and
                len(summary['unique_capture_sha256']) == config['expected_unique_captures'],
                'Corpus totals changed')
        summary.update(completed=True, passed=True)
    except Exception as error:
        summary['error'] = str(error)
        save()
        raise
    save()
    return 0


if __name__ == '__main__':
    sys.exit(main())
