"""Check both shipped Quad entry points against saved Python GPU evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    for name in ('reference', 'matrix', 'bin', 'qt-bin', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.reference / 'standalone'))
    from image import read_png
    jobs = json.loads((args.matrix / 'jobs.json').read_text())
    expected = json.loads((args.matrix / 'expected.json').read_text())
    labels = json.loads((args.matrix / 'labels.json').read_text())
    env = {k: v for k, v in os.environ.items() if k.upper() in
           ('SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'APPDATA', 'LOCALAPPDATA')}
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + str(Path(os.environ['SystemRoot']) / 'System32')
    checks = []
    for executable in ('FloraGPA.Cli.exe', 'FloraGPA.Worker.exe'):
        exe = (args.bin / executable).resolve()
        for label in ('hardware-depth-less-prepared', 'warp-final-gs-before',
                      'hardware-viewport-counter-none', 'hardware-selected-layer-2-before',
                      'hardware-disabled-final-prepared', 'hardware-depth-null_ps-prepared',
                      'hardware-invalid-target-none'):
            index = labels.index(label)
            job, want = jobs[index], expected[index]
            output = (args.out / executable / label).resolve()
            cmd = [str(exe), 'quad', job['capture'], '--id', str(job['event']),
                   '--quad-depth', job['depth'], '--quad-target', job.get('target', 'auto'),
                   '--out', str(output)]
            if job['warp']:
                cmd.append('--warp')
            if 'layer' in job:
                cmd.extend(['--quad-layer', str(job['layer'])])
            if 'experiment' in job:
                cmd.extend(['--experiment', job['experiment']])
            run = subprocess.run(cmd, env=env, capture_output=True, timeout=180)
            output.parent.mkdir(parents=True, exist_ok=True)
            output.with_suffix('.stdout.json').write_bytes(run.stdout)
            output.with_suffix('.stderr.log').write_bytes(run.stderr)
            if 'error' in want:
                actual = json.loads(run.stderr)
                assert run.returncode != 0 and not actual['completed'], label
                assert actual['error'] == f"Event {job['event']} (Draw): " + want['error'], actual
            else:
                assert run.returncode == 0, run.stderr
                report = json.loads(run.stdout)
                assert report['completed'] and report['quad'] == want['report'], label
                assert json.loads((output / 'quad.json').read_text()) == want['report'], label
                data = output / 'data'
                assert json.loads((data / 'result.json').read_text()) == want['counter_report'], label
                assert hashlib.sha256(read_png(data / 'quad_counts.png')[2]).hexdigest() == want['preview_sha256'], label
                assert [hashlib.sha256((data / (name + '.u32le')).read_bytes()).hexdigest()
                        for name in ('locks', 'counts', 'live', 'histogram', 'reference')] == want['storage_sha256'], label
                forbidden = [m for m in report['loaded_modules'] if
                             Path(m).name.lower().startswith(('python', 'gpa-', 'gpa_', 'tk8', 'tcl8')) or
                             Path(m).name.lower() in ('dx11_player.dll', 'dx11_playback.dll', 'shimd3d64.dll', 'gpa.dll') or
                             'intelswtools\\gpa' in m.lower()]
                assert not forbidden, forbidden
            checks.append(dict(executable=executable, label=label, passed=True))
        # Boundary and command-specific flags must fail before diagnostic output.
        capture = jobs[0]['capture']
        for name, command in (
            ('missing-event', ['quad', capture]),
            ('invalid-depth', ['quad', capture, '--id', '100', '--quad-depth', 'invalid']),
            ('negative-layer', ['quad', capture, '--id', '100', '--quad-layer', '-1']),
            ('before-boundary', ['quad', capture, '--id', '100', '--before']),
            ('foreign-options', ['inventory', capture, '--quad-depth', 'none'])):
            output = (args.out / executable / name).resolve()
            run = subprocess.run([str(exe), *command, '--out', str(output)], env=env, capture_output=True, timeout=30)
            assert run.returncode != 0 and not json.loads(run.stderr)['completed'], (name, run.stderr)
            assert not (output / 'quad.json').exists(), name
            checks.append(dict(executable=executable, label=name, passed=True))
    report = dict(passed=True, checks=checks, executables={name: hashlib.sha256((args.bin/name).read_bytes()).hexdigest()
                  for name in ('FloraGPA.Cli.exe', 'FloraGPA.Worker.exe')})
    (args.out / 'validation.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(dict(passed=True, checks=len(checks))))


if __name__ == '__main__':
    main()
