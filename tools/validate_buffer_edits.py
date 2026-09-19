"""Compare event-scoped buffer experiments with the preserved Python GPU replay."""
import argparse
import base64
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--captures', type=Path, required=True)
p.add_argument('--qt-bin', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--only-case', help='Comma-separated case names for focused checks')
args = p.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
env = dict(os.environ)
env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env['PATH']
sys.path.insert(0, str(args.reference.resolve()))
from frame import Frame
from image import read_png

gf2 = args.captures / 'GF2_Exilium_2026_03_03__00_19_35.gpa_frame'
bf1 = args.captures / 'bf1_2026_01_21__16_53_05.gpa_frame'
cases = [('gf2-cb', gf2, 113, 104, 0, struct.pack('<f', 10)),
         ('gf2-vb', gf2, 430, 22, 396*24, struct.pack('<f', -800)),
         ('gf2-ib', gf2, 430, 41, 594*2, struct.pack('<H', 1)),
         ('bf1-srv', bf1, 1415, 859, 0, struct.pack('<I', 10)),
         ('bf1-uav', bf1, 578, 571, 0, struct.pack('<f', .25))]
if args.only_case:
    requested = set(args.only_case.split(','))
    if not requested <= {case[0] for case in cases}: p.error('Unknown case name')
    cases = [case for case in cases if case[0] in requested]
results = []
def run(command, log):
    result = subprocess.run(command, env=env, capture_output=True, timeout=180)
    log.write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f'{log.name}: {result.stderr.decode(errors="replace")[-1800:]}')

for key, capture, event, resource, offset, patch in cases:
    with Frame(capture) as frame:
        digest = hashlib.sha256(frame.bytes).hexdigest()
    op = dict(kind='buffer', event=event, resource=resource, offset=offset,
              asset=dict(data=base64.b64encode(patch).decode(), sha256=hashlib.sha256(patch).hexdigest()))
    project = dict(format='FloraGPA experiment 1', frame_sha256=digest, frame_name=capture.name, cursor=1,
                   history=[dict(label=key, operations=[op]),
                            dict(label='Disable event', operations=[dict(kind='enabled', event=event, value=False)])])
    path = args.out / (key + '.json')
    for variant in ('before', 'after', 'disabled', 'undo'):
        project['cursor'] = {'before':1, 'after':1, 'disabled':2, 'undo':0}[variant]
        path.write_text(json.dumps(project), encoding='utf-8')
        native, reference = (args.out / f'{key}-{variant}-{label}' for label in ('native','reference'))
        shared = ['--event', str(event), '--id', str(resource), '--experiment', str(path)]
        native_cmd = [str(args.exe.resolve()), 'buffer', str(capture), '--out', str(native), *shared]
        reference_cmd = [sys.executable, str(args.reference / 'analyze.py'), str(capture), 'buffer', '--out', str(reference), *shared]
        if variant in ('before','undo'): native_cmd.append('--before')
        else: reference_cmd.append('--after')
        run(native_cmd, args.out / f'{key}-{variant}-native.log')
        run(reference_cmd, args.out / f'{key}-{variant}-reference.log')
        data = (native / 'buffer.bin').read_bytes()
        assert data == (reference / 'buffer.bin').read_bytes(), (key,variant)
        metadata = json.loads((native / 'report.json').read_text())
        expected_metadata = json.loads((reference / 'result.json').read_text())
        bindings = metadata['bindings']
        for binding in bindings:
            if 'view' in binding: binding['view'] = int(binding['view'])
        assert bindings == expected_metadata['bindings'], (key, variant, 'bindings')
        assert metadata['edit_effect'] == expected_metadata['edit_effect']
        assert metadata['edit_effect'] == ('persistent_output' if key == 'bf1-uav' else 'scoped_input')
        if variant == 'before': assert data[offset:offset+len(patch)] == patch
        results.append(dict(case=key, variant=variant, passed=True, sha256=hashlib.sha256(data).hexdigest()))
        print(key, variant, 'PASS', flush=True)
    before = (args.out / f'{key}-before-native/buffer.bin').read_bytes()
    original = (args.out / f'{key}-undo-native/buffer.bin').read_bytes()
    assert before != original, (key,'ineffective patch')
    if key != 'bf1-uav':
        assert (args.out / f'{key}-after-native/buffer.bin').read_bytes() == original
    if key in ('gf2-vb','gf2-ib'):
        project['cursor'] = 1
        path.write_text(json.dumps(project), encoding='utf-8')
        native, reference = (args.out / f'{key}-geometry-{label}' for label in ('native','reference'))
        run([str(args.exe.resolve()), 'geometry', str(capture), '--event', str(event), '--experiment', str(path), '--out', str(native)],
            args.out / f'{key}-geometry-native.log')
        run([sys.executable, str(args.reference / 'analyze.py'), str(capture), 'geometry', '--id', str(event), '--experiment', str(path), '--out', str(reference)],
            args.out / f'{key}-geometry-reference.log')
        for table in ('vertices.csv', 'unique_vertices.csv', 'references.csv'):
            with (native/table).open(newline='') as a, (reference/table).open(newline='') as b:
                for index,(x,y) in enumerate(zip(csv.reader(a), csv.reader(b), strict=True)):
                    for u,v in zip(x,y,strict=True):
                        if u==v: continue
                        assert index and (float(u)==float(v) or math.isnan(float(u)) and math.isnan(float(v))), (key,table,u,v)
        results.append(dict(case=key, variant='geometry', passed=True))
        print(key,'geometry PASS',flush=True)
        native, reference = (args.out / f'{key}-render-{label}' for label in ('native','reference'))
        shared = ['--id','80','--event',str(event),'--experiment',str(path)]
        run([str(args.exe.resolve()),'texture',str(capture),'--out',str(native),*shared],
            args.out/f'{key}-render-native.log')
        run([sys.executable,str(args.reference/'analyze.py'),str(capture),'texture','--after','--out',str(reference),*shared],
            args.out/f'{key}-render-reference.log')
        pixels = (native/'frame.rgba').read_bytes()
        assert pixels == read_png(reference/'preview.png')[2], (key,'render')
        project['cursor'] = 0
        path.write_text(json.dumps(project), encoding='utf-8')
        original_render = args.out / f'{key}-render-original'
        run([str(args.exe.resolve()),'texture',str(capture),'--out',str(original_render),*shared],
            args.out/f'{key}-render-original.log')
        assert pixels != (original_render/'frame.rgba').read_bytes(), (key,'render unchanged')
        project['cursor'] = 1
        path.write_text(json.dumps(project), encoding='utf-8')
        results.append(dict(case=key,variant='render',passed=True,sha256=hashlib.sha256(pixels).hexdigest()))
        print(key,'render PASS',flush=True)
(args.out/'validation.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
