"""Compare archived and current native executors on one captured single-group dispatch.

Runs GPU work serially, checks original GPU comparisons and preserves every raw
report. This is a focused performance/semantic observation, not release acceptance.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', required=True, type=Path)
    parser.add_argument('--after', required=True, type=Path)
    parser.add_argument('--frame', required=True, type=Path)
    parser.add_argument('--qt', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env['PATH'] = str(args.qt.resolve()) + os.pathsep + env.get('PATH', '')
    paths = dict(before=args.before.resolve(), after=args.after.resolve(), frame=args.frame.resolve())
    report = dict(schema=1, full_release_acceptance=False, cases=[],
                  inputs={key: dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
                          for key, path in paths.items()})
    for repeat in range(2):
        for vendor in (0, 4318, 32902):
            # Reverse ordering on the second repetition to expose warm-order effects.
            for version in (('before', 'after') if repeat == 0 else ('after', 'before')):
                label = f'{repeat}-{vendor}-{version}'
                result_path = args.out / (label + '.json')
                command = list(map(str, (paths[version], paths['frame'], 325, 5, vendor, result_path,
                                        '--execute-group', '--compare-native')))
                start = time.monotonic()
                process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                timed_out = False
                try:
                    stdout, stderr = process.communicate(timeout=90)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                                   capture_output=True, check=False, timeout=15)
                    stdout, stderr = process.communicate(timeout=10)
                (args.out / (label + '.stdout')).write_bytes(stdout)
                (args.out / (label + '.stderr')).write_bytes(stderr)
                row = dict(label=label, version=version, vendor=vendor, repeat=repeat, command=command,
                           exit_code=process.returncode, timed_out=timed_out, seconds=time.monotonic() - start)
                if result_path.is_file():
                    data = json.loads(result_path.read_text(encoding='utf-8'))
                    row['execution'] = data.get('execution')
                    row['report_sha256'] = hashlib.sha256(result_path.read_bytes()).hexdigest()
                report['cases'].append(row)
                (args.out / 'matrix.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
                print(json.dumps({key: row[key] for key in ('label', 'exit_code', 'timed_out', 'seconds')}), flush=True)


if __name__ == '__main__':
    main()
