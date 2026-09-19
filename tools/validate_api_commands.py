"""Development oracle for native API wire inspection; never loaded by the application."""
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--captures', type=Path, required=True)
p.add_argument('--fixture', type=Path, action='append', default=[])
p.add_argument('--qt-bin', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
args = p.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
sys.path.insert(0, str(args.reference.resolve()))
from frame import Frame
from api_commands import export, resource_selection

env = dict(os.environ)
env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env['PATH']
results = []
cases = [('gf2', args.captures / 'GF2_Exilium_2026_03_03__00_19_35.gpa_frame', '', None),
         ('bf1', args.captures / 'bf1_2026_01_21__16_53_05.gpa_frame', '', None),
         ('bf1-filter', args.captures / 'bf1_2026_01_21__16_53_05.gpa_frame', 'set decoded', 25733)]
for i, fixture in enumerate(args.fixture):
    cases.append(('synthetic-' + str(i), fixture, '', None))
for name, path, term, resource in cases:
    native, reference = (args.out / (name + '-' + label) for label in ('native', 'reference'))
    cmd = [str(args.exe.resolve()), 'commands', str(path.resolve()), '--out', str(native), '--filter', term]
    if resource is not None: cmd += ['--resource', str(resource)]
    run = subprocess.run(cmd, env=env, capture_output=True, timeout=180)
    (args.out / (name + '.log')).write_bytes(run.stdout + run.stderr)
    if run.returncode: raise RuntimeError(run.stderr.decode(errors='replace'))
    with Frame(path) as frame: expected = export(frame, reference, term, resource)
    actual = json.loads((native / 'commands.json').read_text(encoding='utf-8'))
    errors = []
    for a, b in zip(actual['commands'], expected['commands']):
        # Core reader diagnostics may be worded differently. All decoded fields, offsets,
        # statuses, references, remaining bytes and query provenance must still match.
        if a.get('status') == b.get('status') == 'invalid':
            assert bool(a.get('error')) and bool(b.get('error'))
            a.pop('error'); b.pop('error')
        if a != b:
            errors.append(dict(id=a['id'], native=a, reference=b))
    assert len(actual['commands']) == len(expected['commands'])
    if errors:
        (args.out / (name + '-differences.json')).write_text(json.dumps(errors, ensure_ascii=False, indent=2), encoding='utf-8')
        raise AssertionError((name, len(errors), 'record differences'))
    assert actual == expected
    with (native / 'commands.csv').open(encoding='utf-8-sig', newline='') as f: a = list(csv.reader(f))
    with (reference / 'commands.csv').open(encoding='utf-8-sig', newline='') as f: b = list(csv.reader(f))
    assert a == b, (name, 'CSV differs')
    results.append(dict(case=name, records=len(actual['commands']), passed=True))
    print(name, len(actual['commands']), 'PASS', flush=True)
(args.out / 'validation.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
