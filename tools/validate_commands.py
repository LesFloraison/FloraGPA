"""Development-only Python/native command experiment comparison; GPU runs are serial."""
import argparse
import base64
import hashlib
import json
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
args = p.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
sys.path.insert(0, str(args.reference.resolve()))
from frame import Frame
from image import read_png
from update_sources import describe

env = dict(os.environ)
env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env['PATH']
results = []
gf2 = args.captures / 'GF2_Exilium_2026_03_03__00_19_35.gpa_frame'
bf1 = args.captures / 'bf1_2026_01_21__16_53_05.gpa_frame'
cases = [('gf2-rtv', gf2, 79, 'clear'), ('bf1-rtv', bf1, 23208, 'clear'),
         ('bf1-uav-float', bf1, 23365, 'clear'),
         ('bf1-update-rgba16', bf1, 132, 'update_source'),
         ('bf1-update-rgba8', bf1, 136, 'update_source')]
for key, path, event, kind in cases:
    with Frame(path) as frame:
        operation = dict(kind=kind, event=event)
        if kind == 'clear':
            view = struct.unpack_from('<Q', frame.payload(event), 16)[0]
            resource = struct.unpack_from('<Q', frame.payload(view), 16)[0]
            operation['values'] = dict(values=[.25, .5, .75, 1.])
        else:
            layout = describe(frame, event)
            resource = layout['resource_id']
            data = bytes(layout['size'])
            operation['asset'] = dict(data=base64.b64encode(data).decode(), sha256=hashlib.sha256(data).hexdigest())
        project = dict(format='FloraGPA experiment 1', frame_sha256=hashlib.sha256(frame.bytes).hexdigest(),
                       frame_name=path.name, cursor=1, history=[dict(label=key, operations=[operation])])
        action = 'buffer' if frame.entries[resource].type == 0x83 else 'texture'
    project_path = args.out / (key + '.json')
    project_path.write_text(json.dumps(project), encoding='utf-8')
    for when in ('before', 'after', 'undo'):
        project['cursor'] = 0 if when == 'undo' else 1
        project_path.write_text(json.dumps(project), encoding='utf-8')
        native = args.out / f'{key}-{when}-native'
        reference = args.out / f'{key}-{when}-reference'
        common = ['--id', str(resource), '--event', str(event), '--experiment', str(project_path)]
        native_cmd = [str(args.exe.resolve()), action, str(path), '--out', str(native), *common]
        reference_cmd = [sys.executable, str(args.reference / 'analyze.py'), str(path), action, '--out', str(reference), *common]
        if when == 'before':
            native_cmd += ['--before']
        else:
            reference_cmd += ['--after']
        for label, command in [('native', native_cmd), ('reference', reference_cmd)]:
            run = subprocess.run(command, env=env, capture_output=True, timeout=120)
            (args.out / f'{key}-{when}-{label}.log').write_bytes(run.stdout + run.stderr)
            if run.returncode:
                raise RuntimeError(f'{key}/{when}/{label}: {run.stderr.decode(errors="replace")[-1500:]}')
        filename = 'buffer.bin' if action == 'buffer' else 'frame.rgba'
        pixels = (native / filename).read_bytes()
        expected = (reference / filename).read_bytes() if action == 'buffer' else read_png(reference / 'preview.png')[2]
        assert pixels == expected, (key, when)
        results.append(dict(case=key, boundary=when, passed=True, sha256=hashlib.sha256(pixels).hexdigest()))
        print(key, when, 'PASS', flush=True)
    edited = (args.out / f'{key}-after-native' / filename).read_bytes()
    original = (args.out / f'{key}-undo-native' / filename).read_bytes()
    assert edited != original, (key, 'edit did not change the selected texture')
(args.out / 'validation.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
