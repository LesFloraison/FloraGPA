"""Run serial original class Gather controls with an unrelated t0/s0 binding.

Requires the native class-resource test executable. Nonzero original/native
comparisons remain test failures; this script records rather than waives them.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def shader_code(path):
    data = path.read_bytes()
    count, = struct.unpack_from('<I', data, 28)
    for offset in struct.unpack_from('<' + 'I' * count, data, 32):
        tag, size = struct.unpack_from('<4sI', data, offset)
        if tag == b'SHEX':
            return data[offset + 8:offset + 8 + size]
    raise ValueError('Original control shader has no executable chunk')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.exe = args.exe.resolve()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    cases = [f'{vendor}-0-1-2-{second}' for vendor in (0, 4318, 32902) for second in range(2)]
    runs, observations = [], []
    for decoy in (False, True):
        directory = args.out / ('decoy' if decoy else 'unbound')
        log = args.out / ('decoy.txt' if decoy else 'unbound.txt')
        env = os.environ.copy()
        env['FLORA_INTERFACE_RESOURCE_EVIDENCE'] = str(directory)
        env['FLORA_INTERFACE_RESOURCE_DECOY'] = '1' if decoy else '0'
        command = [str(args.exe), *['original:' + case for case in cases], '-o', str(log) + ',txt']
        start = time.monotonic()
        run = dict(command=command, decoy=decoy, timeout_seconds=60, timed_out=False)
        try:
            result = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            run.update(exit_code=result.returncode, console=result.stdout.decode('utf-8', errors='replace'))
        except subprocess.TimeoutExpired:
            run['timed_out'] = True
        run['elapsed_ms'] = round((time.monotonic() - start) * 1000)
        runs.append(run)
        (args.out / 'runs.json').write_text(json.dumps(runs, indent=2) + '\n')
        if run['timed_out']:
            raise SystemExit('Original Gather control timed out')
        for case in cases:
            report = json.loads((directory / (case + '.json')).read_text())
            if report['decoy'] is not decoy:
                raise ValueError('Control executable did not apply the requested binding variant')
            observations.append(dict(case=case, decoy=decoy, original=report['original'],
                native=report['native'], expected=report['expected'], gpu=report['gpu']))
    for case in cases:
        if shader_code(args.out / 'unbound' / (case + '.dxbc')) != shader_code(args.out / 'decoy' / (case + '.dxbc')):
            raise ValueError('Control changed original executable shader instructions')
    files = sorted(path for path in args.out.rglob('*') if path.is_file()) + [args.exe, Path(__file__)]
    report = dict(schema=1, scope='original_class_gather_unrelated_binding_control',
                  evidence_complete=True, parity_passed=all(row['original'] == row['native'] == row['expected'] for row in observations),
                  identical_executable_chunks=True, observations=observations,
                  files=[dict(path=p.as_posix(), sha256=digest(p)) for p in files])
    (args.out / 'control.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'files'}))
    if not report['parity_passed'] or any(run['exit_code'] for run in runs):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
