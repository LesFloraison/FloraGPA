"""Serial development validation against the pinned cross-frame counter originals.

Missing initial values must reject. Explicit, producer-backed experiment values
must restore both bytes and counts. The optional original observer records the
original player's loss; it is not an oracle for the application's true state.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--exe', type=Path, required=True)
    p.add_argument('--qt-bin', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--original', action='store_true')
    a = p.parse_args()
    root, exe, out = a.reference.resolve(), a.exe.resolve(strict=True), a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    samples = root / 'analysis/capture_samples/preframe_counters2'
    manifest = json.loads((samples / 'manifest.json').read_text())
    for name, expected in manifest['sources'].items():
        if digest(root / name) != expected:
            raise ValueError('Producer source changed: ' + name)
    env = dict(os.environ)
    env.pop('GPA_LOCAL_INJECT', None)
    env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + env['PATH']
    report = dict(completed=False, cases=[], exe_sha256=digest(exe),
                  manifest_sha256=digest(samples / 'manifest.json'),
                  original_adapter='not identified; no matched-device equivalence claim')

    def save():
        (out / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(command, log, expected=0):
        result = subprocess.run(list(map(str, command)), env=env, capture_output=True, timeout=180)
        log.write_bytes(result.stdout + result.stderr)
        if result.returncode != expected:
            raise ValueError(f'Unexpected process result {result.returncode}: {log}')
        return result

    save()
    for case in manifest['cases']:
        capture = samples / case['file']
        assert digest(capture) == case['sha256']
        folder = out / case['name']
        folder.mkdir()
        row = dict(name=case['name'], capture_sha256=case['sha256'], checks=[])
        report['cases'].append(row)
        run([exe, 'validate-frame', capture, '--out', folder / 'preflight'], folder / 'preflight.log', 2)
        preflight = json.loads((folder / 'preflight/validation.json').read_text())
        findings = [f for f in preflight['findings'] if f['kind'] == 'counter_initial_value_missing']
        assert preflight['completed'] and any(f['event_id'] in case['draws'] and
            f['resource_id'] == case['uav_view'] and f['storage_id'] == case['uav_resource'] for f in findings)
        row['missing_counter_findings'] = findings
        failed = run([exe, 'replay', capture, '--out', folder / 'default'], folder / 'default.log', 1)
        messages = []
        for line in (failed.stdout + failed.stderr).decode(errors='replace').splitlines():
            try:
                messages.append(json.loads(line))
            except ValueError:
                pass
        assert any(isinstance(j, dict) and j.get('completed') is False and
                   'no captured frame-initial counter' in j.get('error', '') for j in messages)
        row['default_rejected'] = True
        project = dict(format='FloraGPA experiment 1', frame_sha256=case['sha256'],
                       frame_name=capture.name, cursor=1, history=[dict(label='Producer-backed initial count',
                       operations=[dict(kind='initial_uav_counter', view=case['uav_view'],
                                        value=case['expected_before'])])])
        project_path = folder / 'experiment.json'
        project_path.write_text(json.dumps(project, indent=2) + '\n')
        expected = (samples / case['name'] / 'captured' / f"{case['frame_index']}.uav").read_bytes()
        row['producer_bytes_sha256'] = hashlib.sha256(expected).hexdigest()
        for warp in (False, True):
            for repeat in range(2):
                tag = f"{'warp' if warp else 'hardware'}-{repeat}"
                target = folder / tag
                command = [exe, 'buffer', capture, '--id', case['uav_resource'], '--event', case['draws'][-1],
                           '--experiment', project_path, '--out', target]
                if warp:
                    command += ['--warp']
                run(command, folder / (tag + '.log'))
                native = json.loads((target / 'report.json').read_text())
                assert (target / 'buffer.bin').read_bytes() == expected
                counters = native['uav_counters']
                assert len(counters) == 1 and counters[0]['view'] == case['uav_view']
                assert counters[0]['value'] == case['expected_after']
                row['checks'].append(dict(tag=tag, count=counters[0]['value'], exact_bytes=True))
                save()
        if a.original:
            observer = root / 'tools/probe_original_preframe_counter.py'
            run([sys.executable, observer, capture, '--slot', case['slot'], '--out', folder / 'original'],
                folder / 'original.log')
            original = json.loads((folder / 'original/result.json').read_text())
            audit = original['audit']
            assert not audit['errors'] and len(audit['draws']) == 1
            draw = audit['draws'][0]
            assert draw['before']['counter'] == 0
            assert draw['after']['counter'] == case['expected_after'] - case['expected_before']
            assert bytes.fromhex(draw['after']['buffer_hex']) != expected
            row['original_missing_counter_reproduced'] = True
            row['original_observer_sha256'] = digest(observer)
        save()
        print(case['name'], 'PASS', flush=True)
    report['completed'] = True
    save()


if __name__ == '__main__':
    main()
