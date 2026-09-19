"""Development-only buffer comparison and native CLI rejection checks."""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--captures', type=Path, required=True)
p.add_argument('--qt-bin', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
args = p.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
env = dict(os.environ)
env['PATH'] = str(args.qt_bin) + os.pathsep + env['PATH']
frame = args.captures / 'GF2_Exilium_2026_03_03__00_19_35.gpa_frame'
results = []
cases = [(104, None, False, 0, None), (104, 113, False, 0, None),
         (104, 181, True, 3, 17), (22, 430, False, 16, 128), (41, 430, True, 2, 61)]
for resource, event, after, offset, length in cases:
    key = f'{resource}-{event}-{after}'
    native, reference = args.out / (key + '-native'), args.out / (key + '-reference')
    base = ['--id', str(resource), '--offset', str(offset)]
    if length is not None:
        base += ['--length', str(length)]
    a = [str(args.exe.resolve()), 'buffer', str(frame), '--out', str(native), *base]
    b = [sys.executable, str(args.reference / 'analyze.py'), str(frame), 'buffer', '--out', str(reference), *base]
    if event:
        a += ['--event', str(event)] + ([] if after else ['--before'])
        b += ['--event', str(event)] + (['--after'] if after else [])
    for index, command in enumerate((a, b)):
        result = subprocess.run(command, env=env, capture_output=True, timeout=60)
        (args.out / f'{key}-{index}.log').write_bytes(result.stdout + result.stderr)
        assert result.returncode == 0, result.stderr.decode(errors='replace')
    data = (native / 'buffer.bin').read_bytes()
    assert data == (reference / 'buffer.bin').read_bytes()
    with (native / 'words.csv').open(newline='') as left, (reference / 'words.csv').open(newline='') as right:
        for row, (x, y) in enumerate(zip(csv.reader(left), csv.reader(right), strict=True)):
            for u, v in zip(x, y, strict=True):
                if u == v:
                    continue
                assert row and (float(u) == float(v) or math.isnan(float(u)) and math.isnan(float(v))), (u, v)
    results.append({'case': key, 'passed': True, 'sha256': hashlib.sha256(data).hexdigest()})
    print(key, 'PASS', flush=True)
for name, command, options in [
    ('wrong-resource', 'buffer', ['--id', '123']),
    ('range-overrun', 'buffer', ['--id', '104', '--offset', '176', '--length', '1']),
    ('invalid-event-zero', 'buffer', ['--id', '104', '--event', '0']),
    ('index-overflow', 'texture', ['--id', '123', '--mip', '4294967296']),
]:
    result = subprocess.run([str(args.exe.resolve()), command, str(frame), '--out', str(args.out / name), *options],
                            env=env, capture_output=True, timeout=60)
    assert result.returncode == 1, (name, result.returncode)
    error = json.loads(result.stderr.splitlines()[-1])
    assert error['completed'] is False and error['error'], (name, error)
    results.append({'case': name, 'passed': True, 'error': error['error']})
(args.out / 'validation.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
