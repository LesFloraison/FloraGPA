"""Exercise the packaged repeated-profile CLI, worker transport and rejected requests."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    env = {key: value for key, value in os.environ.items()
           if key.upper() in {'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA'}}
    windows = Path(os.environ['SystemRoot'])
    env['PATH'] = str(windows / 'System32') + os.pathsep + str(windows)
    checks = []
    cases = [
        ('draws', [], [100, 200], True),
        ('writes', ['--include-writes'], [100, 150, 160, 200], True),
        ('range-id', ['--id', '150', '--end-event', '160', '--include-writes', '--warp'], [150, 160], True),
        ('range-alias', ['--start-event', '150', '--end-event', '160', '--include-writes', '--warp'], [150, 160], True),
        ('disabled', ['--disable', '100,160', '--include-writes'], [100, 150, 160, 200], True),
        ('zero-samples', ['--samples', '0'], [], False),
        ('excess-samples', ['--samples', '1001'], [], False),
        ('negative-warmup', ['--warmup', '-1'], [], False),
        ('missing-id', ['--id', '99'], [], False),
        ('reversed', ['--id', '200', '--end-event', '100'], [], False),
        ('no-work', ['--id', '150', '--end-event', '160'], [], False),
        ('two-starts', ['--id', '100', '--start-event', '100'], [], False),
        ('before', ['--before'], [], False),
    ]
    for name, extra, events, success in cases:
        folder = args.out / name
        exe = args.package.resolve() / ('FloraGPA.Worker.exe' if name == 'writes' else 'FloraGPA.Cli.exe')
        command = [str(exe), 'timings', str(args.capture.resolve()), '--samples', '2', '--warmup', '1',
                   '--out', str(folder), *extra]
        run = subprocess.run(command, env=env, capture_output=True, timeout=180)
        (args.out / (name + '.log')).write_bytes(run.stdout + run.stderr)
        assert (run.returncode == 0) == success, name
        if success:
            profile = json.loads((folder / 'profile.json').read_text('utf-8'))
            report = json.loads((folder / 'report.json').read_text('utf-8'))
            assert report['completed'] and report['loaded_modules'], name
            assert profile['requested_samples'] == 2 and profile['warmup_replays'] == 1, name
            assert [row['event'] for row in profile['events']] == events, name
            assert [p['replay_generation'] for p in profile['passes']] == [2, 3], name
            assert all(report[key] == value for key, value in profile.items()), name
            if name == 'disabled':
                assert [row['event'] for row in profile['events'] if not row['enabled']] == [100, 160]
            if name.startswith('range-'):
                assert profile['execution_device']['selected'] == 'warp'
                assert profile['range'] == {'start': 150, 'end': 160, 'inclusive': True}
        else:
            assert not (folder / 'profile.json').exists(), name
        checks.append(dict(case=name, passed=True))
    report = dict(passed=True, cases=len(checks), checks=checks)
    (args.out / 'validation.json').write_text(json.dumps(report, indent=2), 'utf-8')
    print(json.dumps(dict(passed=True, cases=len(checks))))


if __name__ == '__main__':
    main()
