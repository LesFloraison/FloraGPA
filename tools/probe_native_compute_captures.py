"""Run bounded, serial native compute jobs on explicitly selected captured dispatches.

A successful job proves completion and artifact production, not arithmetic parity.
No external analyzer is loaded or treated as an acceptance oracle.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary-directory', type=Path, required=True)
    parser.add_argument('--qt', type=Path, required=True)
    parser.add_argument('--case', nargs=2, action='append', required=True, metavar=('FRAME', 'EVENT'))
    parser.add_argument('--vendor', type=int, default=4318)
    parser.add_argument('--timeout', type=int, default=90)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.timeout < 1 or args.timeout > 600 or args.vendor < 0:
        parser.error('Invalid timeout or vendor')
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    binaries = args.binary_directory.resolve()
    env = os.environ.copy()
    env['PATH'] = str(args.qt.resolve()) + os.pathsep + env.get('PATH', '')
    env['QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES'] = '1'
    summary = dict(schema=1, scope='captured_native_compute_job_observations', full_parity=False,
                   binaries=[dict(path=str(binaries / name), sha256=digest(binaries / name))
                             for name in ('FloraGPA.Cli.exe', 'FloraGPA.Worker.exe')], cases=[])
    hashes = {}
    for index, (name, event_text) in enumerate(args.case):
        frame, event = Path(name).resolve(), int(event_text)
        if event < 0 or not frame.is_file():
            raise ValueError('Invalid frame or event')
        if frame not in hashes:
            hashes[frame] = digest(frame)
        output = args.out / str(index)
        command = [str(binaries / 'FloraGPA.Cli.exe'), 'analyze', str(frame), 'debug-thread',
                   '--gpa-event', str(event), '--group', '0', '0', '0', '--thread', '0', '0', '0',
                   '--timeout', str(args.timeout), '--out', str(output)]
        command += ['--gpu-vendor', str(args.vendor)] if args.vendor else ['--warp']
        started = time.monotonic()
        process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        timed_out = False
        try:
            stdout, stderr = process.communicate(timeout=args.timeout + 20)
        except subprocess.TimeoutExpired:
            timed_out = True
            subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                           capture_output=True, check=False, timeout=15)
            stdout, stderr = process.communicate(timeout=10)
        (args.out / f'{index}.stdout').write_bytes(stdout)
        (args.out / f'{index}.stderr').write_bytes(stderr)
        result_path = output / 'result.json'
        result = json.loads(result_path.read_text(encoding='utf-8')) if result_path.is_file() else None
        row = dict(index=index, frame=str(frame), frame_sha256=hashes[frame], event=event,
                   vendor=args.vendor, command=command, exit_code=process.returncode,
                   timed_out=timed_out, seconds=time.monotonic() - started,
                   result=result, stderr=stderr.decode('utf-8', errors='replace'))
        summary['cases'].append(row)
        (args.out / 'matrix.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
        print(json.dumps({key: row[key] for key in ('index', 'event', 'exit_code', 'timed_out', 'seconds', 'stderr')}),
              flush=True)


if __name__ == '__main__':
    main()
