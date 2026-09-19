"""Development-only comparison against the preserved Python implementation."""
import argparse
import csv
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
p.add_argument('--only-case', help='Run one case, for example bf1-1415')
args = p.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
env = dict(os.environ)
env['PATH'] = str(args.qt_bin) + os.pathsep + env['PATH']
cases = [('GF2_Exilium_2026_03_03__00_19_35.gpa_frame', 'gf2', 113),
         ('GF2_Exilium_2026_03_03__00_19_35.gpa_frame', 'gf2', 430),
         ('bf1_2026_01_21__16_53_05.gpa_frame', 'bf1', 876),
         ('bf1_2026_01_21__16_53_05.gpa_frame', 'bf1', 1415),
         ('bf1_2026_01_21__16_53_05.gpa_frame', 'bf1', 20471),
         ('bf1_2026_01_21__16_53_05.gpa_frame', 'bf1', 20531)]
if args.only_case:
    cases = [case for case in cases if f'{case[1]}-{case[2]}' == args.only_case]
    if not cases:
        p.error('Unknown geometry case')
results = []
for capture, name, event in cases:
    key = f'{name}-{event}'
    native, reference = args.out / (key + '-native'), args.out / (key + '-reference')
    commands = [
        [str(args.exe.resolve()), 'geometry', str(args.captures / capture), '--event', str(event), '--out', str(native)],
        [sys.executable, str(args.reference / 'analyze.py'), str(args.captures / capture), 'geometry', '--id', str(event), '--out', str(reference)],
    ]
    for index, command in enumerate(commands):
        result = subprocess.run(command, env=env, capture_output=True, timeout=180)
        (args.out / f'{key}-{index}.log').write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f'{key} failed: ' + result.stderr.decode('utf-8', errors='replace'))
    a, b = (json.loads((directory / 'geometry.json').read_text('utf-8')) for directory in (native, reference))
    for field in ('effective_parameters', 'topology', 'elements', 'vertex_references', 'unique_vertices',
                  'strip_restart_references', 'obj_vertices', 'obj_faces', 'obj_lines', 'obj_points'):
        assert a[field] == b[field], (key, field)
    for table in ('vertices.csv', 'unique_vertices.csv', 'references.csv'):
        with (native / table).open(newline='', encoding='utf-8') as left, (reference / table).open(newline='', encoding='utf-8') as right:
            for row, (x, y) in enumerate(zip(csv.reader(left), csv.reader(right), strict=True)):
                for column, (u, v) in enumerate(zip(x, y, strict=True)):
                    if u == v:
                        continue
                    assert row > 0, (key, table, 'header', column)
                    u, v = float(u), float(v)
                    assert u == v or math.isnan(u) and math.isnan(v), (key, table, row, column, u, v)
    # OBJ decimal formatting can differ, but topology and positions must agree.
    if a['obj_vertices']:
        with (native / 'geometry.obj').open() as left, (reference / 'geometry.obj').open() as right:
            for x, y in zip(left, right, strict=True):
                if x.startswith('#'):
                    continue
                u, v = x.split(), y.split()
                assert u[0] == v[0] and len(u) == len(v), key
                if u[0] == 'v':
                    assert all(math.isclose(float(i), float(j), rel_tol=1e-8, abs_tol=1e-8) for i, j in zip(u[1:], v[1:])), key
                else:
                    assert u == v, (key, u, v)
    results.append({'case': key, 'passed': True, 'references': a['vertex_references'], 'unique': a['unique_vertices']})
    print(key, 'PASS', flush=True)
(args.out / 'validation.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
