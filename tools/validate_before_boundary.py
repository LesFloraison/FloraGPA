"""Compare ordinary frame boundaries and prepared inspectors with the Python reference."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--qt-bin', type=Path)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--real-only', action='store_true', help='Compare first/middle/last draws of GF2 and BF1')
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference/'standalone'), str(a.reference/'tools')]
from frame import Frame
from dx11 import Device
from engine import Engine
from experiments import Experiment
from presentation import select
from replay_pipeline import inspect
from validate_buffer_edits import Fixture, graphics_fixture, compute_fixture

env = {k: v for k, v in os.environ.items() if k.upper() in
       {'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA'}}
env['PATH'] = os.environ['WINDIR']+'/System32;'+os.environ['WINDIR']
if a.qt_bin:
    env['PATH'] = str(a.qt_bin.resolve())+os.pathsep+env['PATH']
checks = []
exe_hash = hashlib.sha256(a.exe.read_bytes()).hexdigest()

def record(name, passed, **detail):
    checks.append(dict(name=name, passed=passed, **detail))
    save(False)
    print(name, 'PASS' if passed else 'FAIL', flush=True)
    assert passed, (name, detail)

def save(completed):
    (a.out/'validation.json').write_text(json.dumps(dict(completed=completed,
        passed=all(x['passed'] for x in checks), executable_sha256=exe_hash, checks=checks), indent=2))

def compare(path, driver, event, before, action='replay', target='auto', project=None):
    name = f'{path.stem}-{driver}-{event}-{before}-{action}-{target}'
    if project: name += '-'+project.stem
    destination = a.out/name
    expected = error = pixels = storage = meta = None
    with Frame(path) as frame:
        device = Device(driver)
        try:
            engine = Engine(frame, device, experiment=Experiment(frame, project) if project else None)
            try:
                if action == 'replay-pipeline':
                    expected = inspect(engine, event, after=not before)
                else:
                    engine.replay(event, before=before, readback=False)
                    expected = select(engine, event, target)
                    if expected['resource'] is not None:
                        _, _, pixels = engine.readback(expected['resource'], view=expected['view'])
                        storage, meta = engine.output_storage, engine.output_display
            except (ValueError, RuntimeError, KeyError) as caught:
                error = str(caught)
        finally:
            device.close()
    args = [str(a.exe.resolve()), action, str(path.resolve()), '--event', str(event),
            '--out', str(destination.resolve())]
    if driver == 'warp': args += ['--warp']
    if before: args += ['--before']
    if project: args += ['--experiment', str(project.resolve())]
    if action == 'replay': args += ['--output-target', target]
    run = subprocess.run(args, env=env, capture_output=True, timeout=180)
    (a.out/(name+'.log')).write_bytes(run.stdout+run.stderr)
    if error:
        assert path.stem.startswith('gap-') and event == 150 and not before, (name, error)
        record(name, run.returncode != 0, expected_error=error)
        return
    assert run.returncode == 0, (name, run.stderr.decode(errors='replace'))
    report = json.loads((destination/'report.json').read_text())
    differences = {}
    if action == 'replay-pipeline':
        actual = json.loads((destination/'replay-pipeline.json').read_text())
        for key in ('event', 'api', 'context', 'value_time', 'fields', 'command_enabled',
                    'known_fields', 'unknown_fields', 'experiment_applied'):
            if actual.get(key) != expected[key]:
                differences[key] = dict(actual=actual.get(key), expected=expected[key])
    else:
        if report['output_selection'] != expected:
            differences['selection'] = dict(actual=report['output_selection'], expected=expected)
        if report['image_available'] != (pixels is not None): differences['availability'] = True
        if pixels is not None:
            if (destination/'frame.rgba').read_bytes() != pixels: differences['pixels'] = True
            if (destination/'output_storage.bin').read_bytes() != storage: differences['storage'] = True
            if report['output_display'] != meta: differences['display'] = True
        elif (destination/'frame.png').exists(): differences['fabricated_image'] = True
    if report['reference_pixels_used'] or any(Path(m).name.lower().startswith(
            ('python', 'gpa_', 'gpa-', 'tk8', 'tcl8', 'renderdoc')) for m in report['loaded_modules']):
        differences['runtime'] = True
    (destination/'expected.json').write_text(json.dumps(expected, indent=2))
    record(name, not differences, differences=differences)

if a.real_only:
    from events import DRAW_TYPES
    for filename in ('GF2_Exilium_2026_03_03__00_19_35.gpa_frame', 'bf1_2026_01_21__16_53_05.gpa_frame'):
        path = a.reference/filename
        with Frame(path) as frame:
            draws = sorted(e.id for e in frame.entries.values() if e.category == 7 and e.type in DRAW_TYPES)
        for event in (draws[0], draws[len(draws)//2], draws[-1]):
            for before in (True, False):
                compare(path, 'hardware', event, before)
    save(True)
    print(len(checks), 'real capture checks passed', flush=True)
    sys.exit(0)

base = graphics_fixture(a.out/'base.gpa_frame')
with Frame(base) as frame:
    records = [(e.id, e.category, e.type, bytes(frame.payload(e.id))) for e in frame.entries.values()]

def variant(name, gap=None):
    fixture = Fixture()
    fixture.records = []
    for rid, category, kind, data in records:
        if rid == 199:
            data = bytearray(data)
            struct.pack_into('<Q', data, 17420, 64)
        fixture.add(rid, category, kind, data)
    fixture.add(60, 5, 0x85, bytes(16)+struct.pack('<11IQ', 8, 8, 1, 1, 28, 1, 0, 0, 32, 0, 0, 61))
    fixture.data(61, bytes([32, 64, 96, 255])*64)
    fixture.view(64, 'rtv', 60, [28, 4, 0, 0, 0])
    if gap == 'output':
        fixture.add(150, 7, 0x34ff, struct.pack('<QQIBQQ', 0, 1, 1, 1, 999999, 0))
    elif gap == 'layout':
        fixture.add(150, 7, 0x34ef, struct.pack('<QQQ', 0, 1, 999999))
    elif gap == 'srv':
        fixture.add(150, 7, 0x34e6, struct.pack('<QQIIBQ', 0, 1, 0, 1, 1, 999999))
    else:
        fixture.add(150, 7, 0x34ff, struct.pack('<QQIBQQ', 0, 1, 1, 1, 44, 0))
    if name == 'mapped-write':
        fixture.data(70, struct.pack('<4f', .25, .5, .75, 1))
        for event, hr, kind, data in ((175, 0, 4, 70), (180, -1, 4, 70), (190, 0, 1, 0)):
            fixture.add(event, 7, 0x246, struct.pack('<QQiQIIIQ', 0, 1, hr, 24, 0, kind, 0, data))
    return fixture.write(a.out/(name+'.gpa_frame'))

paths = [variant('changed-target'), variant('mapped-write'),
         *(variant('gap-'+kind, kind) for kind in ('output','layout','srv'))]
paths.append(compute_fixture(a.out/'compute.gpa_frame'))
for path in paths:
    patch = struct.pack('<4f', .25, .5, .75, 1) if path.stem != 'compute' else struct.pack('<I', 77)
    resource = 24 if path.stem != 'compute' else 26
    project = path.with_suffix('.experiment.json')
    project.write_text(json.dumps(dict(format='FloraGPA experiment 1',
        frame_sha256=hashlib.sha256(path.read_bytes()).hexdigest(), cursor=1,
        history=[dict(label='Selected draw input and disable', operations=[
            dict(kind='buffer', event=200, resource=resource, offset=0,
                 asset=dict(data=base64.b64encode(patch).decode(), sha256=hashlib.sha256(patch).hexdigest())),
            dict(kind='enabled', event=200, value=False)])])))
    for driver in ('warp', 'hardware'):
        for event in ((100, 200) if path.stem == 'compute' else (100, 150, 200)):
            for before in (True, False):
                compare(path, driver, event, before)
        for target in ('auto', 'rt0', 'rt7'):
            compare(path, driver, 200, True, target=target, project=project)
        for project_path in (None, project):
            compare(path, driver, 200, True, action='replay-pipeline', project=project_path)
save(True)
print(len(checks), 'checks passed', flush=True)
