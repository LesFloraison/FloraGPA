"""Exercise saved captured compute jobs through the native reader and session exporter."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('jobs', type=Path)
    parser.add_argument('--binary-directory', required=True, type=Path)
    parser.add_argument('--qt', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    jobs = args.jobs.resolve()
    matrix = json.loads((jobs / 'matrix.json').read_text(encoding='utf-8'))
    env = os.environ.copy()
    env['PATH'] = str(args.qt.resolve()) + os.pathsep + env.get('PATH', '')
    binaries = args.binary_directory.resolve()
    names = ('FloraDxbcTraceReaderProbe.exe', 'FloraNativeDebugSessionProbe.exe')
    report = dict(schema=1, jobs=str(jobs), full_parity=False, cases=[],
                  binaries=[dict(path=str(binaries / name),
                                 sha256=hashlib.sha256((binaries / name).read_bytes()).hexdigest())
                            for name in names])
    for row in matrix['cases']:
        if row['exit_code'] != 0 or row['timed_out'] or not row['result']['ok']:
            raise ValueError('Cannot inspect an unsuccessful debug job')
        index = row['index']
        directory = jobs / str(index)
        for kind, command in (
            ('reader', [binaries / names[0], directory / 'trace.jsonl',
                        row['result']['artifacts']['trace.jsonl']['sha256'],
                        args.out / f'{index}.reader.json', '--shader', directory / 'shader.dxbc']),
            ('session', [binaries / names[1], directory, args.out / f'{index}.session.json',
                         args.out / f'{index}.zip'])):
            command = list(map(str, command))
            started = time.monotonic()
            process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            timed_out = False
            try:
                stdout, stderr = process.communicate(timeout=180)
            except subprocess.TimeoutExpired:
                timed_out = True
                subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                               capture_output=True, check=False, timeout=15)
                stdout, stderr = process.communicate(timeout=10)
            (args.out / f'{index}.{kind}.stdout').write_bytes(stdout)
            (args.out / f'{index}.{kind}.stderr').write_bytes(stderr)
            observation = dict(index=index, kind=kind, command=command, seconds=time.monotonic() - started,
                               exit_code=process.returncode, timed_out=timed_out,
                               stderr=stderr.decode('utf-8', errors='replace'))
            report['cases'].append(observation)
            (args.out / 'matrix.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
            print(json.dumps(observation), flush=True)


if __name__ == '__main__':
    main()
