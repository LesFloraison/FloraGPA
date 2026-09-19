"""Development-only golden replay checks. Never imported by FloraGPA binaries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--exe', type=Path, required=True)
parser.add_argument('--captures', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--qt-bin', type=Path)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
env = dict(os.environ)
if args.qt_bin:
    env['PATH'] = str(args.qt_bin) + os.pathsep + env['PATH']
cases = [
    ('GF2_Exilium_2026_03_03__00_19_35.gpa_frame', 'gf2',
     '2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1', 72, 0, 44),
    ('bf1_2026_01_21__16_53_05.gpa_frame', 'bf1',
     '1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6', 1202, 73, 4151),
]
results = []
for capture, name, expected, draws, dispatches, maps in cases:
    for negative in (False, True):
        key = name + ('-negative' if negative else '')
        target = args.out / key
        cmd = [str(args.exe.resolve()), 'replay', str((args.captures / capture).resolve()), '--out', str(target.resolve())]
        if negative:
            cmd.append('--suppress-draws')
        result = subprocess.run(cmd, env=env, capture_output=True, timeout=180)
        (args.out / (key + '.log')).write_bytes(result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.decode('utf-8', errors='replace'))
        report = json.loads((target / 'report.json').read_text('utf-8'))
        actual = hashlib.sha256((target / 'frame.rgba').read_bytes()).hexdigest()
        assert actual == report['rgba_sha256']
        assert (actual != expected) if negative else (actual == expected), key
        assert report['reference_pixels_used'] is False
        modules = [Path(x).name.lower() for x in report['loaded_modules']]
        assert not any(x.startswith(('python', 'gpa-', 'gpa_', 'tk8', 'tcl8')) or x == 'renderdoc.dll' for x in modules), modules
        counts = report['counts']
        assert sum(v for k, v in counts.items() if k.startswith('Draw')) == (0 if negative else draws)
        assert sum(v for k, v in counts.items() if k.startswith('Dispatch')) == dispatches
        assert counts['Map'] == maps
        results.append(dict(case=key, rgba_sha256=actual, passed=True, counts=counts))
        print(key, 'PASS', flush=True)
(args.out / 'validation.json').write_text(json.dumps(results, indent=2) + '\n')
