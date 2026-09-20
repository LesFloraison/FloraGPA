"""Development-only native output/SO replay comparisons with the Python oracle."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import csv
import math
from contextlib import nullcontext

p = argparse.ArgumentParser()
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--qt-bin', type=Path)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--captures', type=Path)
p.add_argument('--quick', action='store_true')
p.add_argument('--extended-only', action='store_true')
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference / 'standalone'), str(a.reference / 'tools')]
from frame import Frame
from engine import Engine
from devices import create_device
from replay_pipeline import inspect as inspect_pipeline
from validate_buffer_edits import Fixture, experiment, operation
from validate_output_setters import fixture, collateral_fixture, ia_fixture, counter_fixture
from validate_so_setters import fixture as so_fixture
from output_setters import captured
from validate_output_commands import rt, cs, om, KEEP, array
from binding_model import FIELDS
from events import inspection_event, DRAW_TYPES
from state import decode_state
from uav_counters import snapshot as counter_snapshot, referenced_snapshot

env = {k: v for k, v in os.environ.items() if k.upper() in {'SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}}
env['PATH'] = os.environ['WINDIR'] + '/System32;' + os.environ['WINDIR']
if a.qt_bin: env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + env['PATH']
checks = []

def save(completed=False):
    (a.out / 'validation.json').write_text(json.dumps(dict(completed=completed, passed=completed and all(c['passed'] for c in checks), checks=checks), indent=2))

def check(name, value, **details):
    checks.append(dict(name=name, passed=bool(value), **details)); save()
    assert value, checks[-1]
    print(name, 'PASS', flush=True)

def native(name, path, action, driver, event, project, before=False, resource=None, fail=False):
    out = a.out / name
    command = [str(a.exe.resolve()), action, str(path.resolve()), '--out', str(out.resolve()), '--experiment', str(project.resolve())]
    if event: command += ['--event', str(event)]
    if before: command.append('--before')
    if driver == 'warp': command.append('--warp')
    if resource is not None: command += ['--id', str(resource)]
    result = subprocess.run(command, env=env, capture_output=True, timeout=180)
    (a.out / (name + '.log')).write_bytes(result.stdout + result.stderr)
    if fail:
        assert result.returncode == 1 and b'"completed":false' in result.stderr, (name, result.stderr)
        return None
    assert result.returncode == 0, (name, result.stderr)
    report = json.loads((out / 'report.json').read_text())
    assert report['completed'] and report['reference_pixels_used'] is False
    assert not any(Path(m).name.lower().startswith(('python', 'gpa_', 'gpa-', 'tk8', 'tcl8')) or Path(m).name.lower() == 'renderdoc.dll' for m in report['loaded_modules'])
    return out, report

def normalized(state):
    rows = sorted((v for v in state['fields'] if v['field'] in FIELDS), key=lambda v: v['field'])
    assert len(rows) == len(FIELDS)
    groups = {}; out = {}
    for row in rows:
        value = json.loads(json.dumps(row)); obj = value.get('object')
        if obj:
            token = str(value['value']); groups.setdefault(token, len(groups) + 1)
            value['value'] = groups[token]
            obj.pop('token', None); obj.pop('runtime_token', None)
            obj['runtime_object'] = 'object-' + str(groups[token])
        out[value['field']] = value
    return out

def compare(name, path, ops, driver, event=100, before=False, resources=(), fail=False):
    label = name + '-' + driver + '-' + str(event) + ('-before' if before else '')
    project = a.out / (label + '.json')
    error = None; expected = {}; counters = {}
    with Frame(path) as f:
        try:
            exp = experiment(f, project, ops); d = create_device(driver)
            try:
                engine = Engine(f, d, experiment=exp)
                engine.replay(event, before=before, readback=False)
                state = inspect_pipeline(Engine(f, d, experiment=exp), event, after=not before)
                # A fresh replay avoids inspection helpers changing resource storage.
                engine = Engine(f, d, experiment=exp); engine.replay(event, before=before, readback=False)
                event_info = inspection_event(f, event)
                effective = engine.setter_tracker.state(decode_state(f.payload(event_info['state_id'], 3, 3))) if 'state_id' in event_info else None
                with exp.inputs(engine, event_info) if before and effective is not None else nullcontext():
                    for kind, resource in resources:
                        expected[resource] = engine.read_buffer(resource) if kind == 'buffer' else engine.read_texture(resource)
                        if kind == 'buffer':
                            counters[resource] = counter_snapshot(engine, event_info, effective, resource) if effective is not None else referenced_snapshot(engine, event_info, resource)
            finally: d.close()
        except (ValueError, RuntimeError, KeyError) as ex:
            if not fail: raise
            error = str(ex)
            if not project.exists(): project.write_text(json.dumps(dict(format='FloraGPA experiment 1', frame_sha256=hashlib.sha256(path.read_bytes()).hexdigest(), frame_name=path.name, cursor=1, history=[dict(label='invalid', operations=ops)])))
    if fail:
        assert error, (label, 'reference accepted unexpectedly')
        native(label, path, 'replay-pipeline', driver, event, project, before, fail=True)
        check(label, True, reference_error=error); return
    out, report = native(label + '-pipeline', path, 'replay-pipeline', driver, event, project, before)
    wanted = normalized(state); actual = normalized(json.loads((out / 'replay-pipeline.json').read_text()))
    differences = {k: [wanted[k], actual[k]] for k in wanted if wanted[k] != actual[k]}
    if differences: (a.out / (label + '-mismatch.json')).write_text(json.dumps(differences, indent=2))
    check(label + '-bindings', not differences, fields=len(FIELDS), sha256=hashlib.sha256(json.dumps(actual, sort_keys=True).encode()).hexdigest())
    for kind, resource in resources:
        out, report = native(label + '-' + str(resource), path, kind, driver, event, project, before, resource)
        data = (out / ('buffer.bin' if kind == 'buffer' else 'frame.rgba')).read_bytes()
        check(label + '-data-' + str(resource), data == expected[resource], expected_sha256=hashlib.sha256(expected[resource]).hexdigest(), actual_sha256=hashlib.sha256(data).hexdigest())
        if kind == 'buffer':
            check(label + '-counters-' + str(resource), report['uav_counters'] == counters[resource], expected=counters[resource], actual=report['uav_counters'])

def setter(event, kind, raw): return dict(kind='setter', event=event, values=captured(kind, raw))

def geometry(name, path, ops, driver, event=100):
    from geometry import export_geometry
    label = name + '-' + driver
    project = a.out / (label + '.json')
    reference = a.out / (label + '-reference'); reference.mkdir()
    with Frame(path) as f:
        exp = experiment(f, project, ops)
        d = create_device(driver)
        try: expected = export_geometry(Engine(f, d, experiment=exp), event, reference)
        finally: d.close()
    out, _ = native(label + '-native', path, 'geometry', driver, event, project)
    actual = json.loads((out / 'geometry.json').read_text())
    for key in ['effective_parameters', 'topology', 'elements', 'vertex_references', 'unique_vertices',
                'strip_restart_references', 'obj_vertices', 'obj_faces', 'obj_lines', 'obj_points']:
        assert actual[key] == expected[key], (label, key)
    for table in ['vertices.csv', 'unique_vertices.csv', 'references.csv']:
        with (out / table).open(newline='') as left, (reference / table).open(newline='') as right:
            for row, (x, y) in enumerate(zip(csv.reader(left), csv.reader(right), strict=True)):
                for u, v in zip(x, y, strict=True):
                    if u == v: continue
                    assert row > 0
                    u, v = float(u), float(v)
                    assert u == v or math.isnan(u) and math.isnan(v), (label, table, row, u, v)
    check(label, True, vertices=actual['vertex_references'])
if not a.extended_only:
    path = fixture(a.out / 'outputs.gpa_frame')
    ops = [setter(80, 0x34ff, rt([234], 0)), setter(85, 0x25e, cs(63, [224], [11]))]
    for driver in ['hardware', 'warp']:
        for event, before in ([(100, False)] if a.quick else [(80, False), (85, False), (100, True), (100, False), (110, False), (120, False), (200, False)]):
            compare('output', path, ops, driver, event, before, resources=[('texture', 30), ('texture', 230)] if event >= 100 else [])
        compare('output-disabled', path, ops + [dict(kind='enabled', event=100, value=False)], driver, resources=[('texture', 230)])
        if not a.quick: compare('output-undo', path, [], driver, resources=[('texture', 30)])

    for version in ([4] if a.quick else range(5)):
        path = so_fixture(a.out / ('so-' + str(version) + '.gpa_frame'), version)
        values = [('offset', dict(count=1, buffers=[40], offsets=[64])), ('target', dict(count=1, buffers=[42], offsets=[64])),
                  ('append', dict(count=1, buffers=[40], offsets=[KEEP])), ('null-offsets', dict(count=1, buffers=[40], offsets=None)),
                  ('unbind', dict(count=0, buffers=None, offsets=None)), ('empty', dict(count=0, buffers=[], offsets=[])),
                  ('null-slot', dict(count=1, buffers=[0], offsets=[KEEP]))]
        for name, value in (values[:2] if a.quick else values):
            edits = [dict(kind='setter', event=120, values=value)]
            for driver in ['hardware', 'warp']:
                for event, before in ([(175, False)] if a.quick else [(120, False), (150, True), (150, False), (175, False), (200, False)]):
                    compare('so-' + str(version) + '-' + name, path, edits, driver, event, before, resources=[('buffer', 40), ('buffer', 42)])
        if not a.quick:
            for driver in ['hardware', 'warp']:
                compare('so-unknown-' + str(version), path, [dict(kind='setter', event=120, values=dict(count=1, buffers=[42], offsets=[KEEP]))], driver, 150, fail=True)

    if not a.quick:
        for name, offsets in [('reset', (256,)), ('append', (KEEP,)), ('null', None)]:
            path = so_fixture(a.out / ('so-later-' + name + '.gpa_frame'), later=offsets)
            for driver in ['hardware', 'warp']:
                compare('so-later-' + name, path, [dict(kind='setter', event=120, values=dict(count=1, buffers=[40], offsets=[64]))], driver, 175, resources=[('buffer', 40)])
        for stage in ['vs', 'hs', 'ds', 'gs', 'ps', 'cs']:
            path = collateral_fixture(a.out / ('collateral-' + stage + '.gpa_frame'), stage)
            edits = [setter(88, 0x25e, cs(1, [0]))]
            for driver in ['hardware', 'warp']:
                compare('collateral-' + stage, path, edits, driver, resources=[('buffer', 74)] if stage == 'cs' else [('texture', 30)])
        path = ia_fixture(a.out / 'ia.gpa_frame')
        for driver in ['hardware', 'warp']:
            compare('ia', path, [setter(85, 0x25e, cs(0, [0, 0]))], driver, resources=[('texture', 30)])

if not a.quick:
    source = counter_fixture(a.out / 'counter-base.gpa_frame')
    x = Fixture(); x.records = []
    with Frame(source) as f:
        for en in f.entries.values():
            raw = f.payload(en.id)
            if en.category == 7 and en.type == 0x3f:
                # Legacy synthetic CopyStructureCount records lack an owner.
                # Give both engines the same explicit immediate context.
                raw = bytearray(raw); struct.pack_into('<Q', raw, 8, 1)
            x.add(en.id, en.category, en.type, raw)
    path = x.write(a.out / 'counter.gpa_frame')
    ops = [setter(80, 0x3522, cs(0, [62], [2]))]
    for driver in ['hardware', 'warp']:
        for event, before in [(80, False), (100, True), (100, False), (200, False)]:
            compare('counter', path, ops, driver, event, before, resources=[('buffer', 60)])
        for reverse in [False, True]:
            patches = [dict(kind='uav_counter', event=100, view=62, value=5), operation(60, 0, struct.pack('<I', 99))]
            combined = patches + ops if reverse else ops + patches
            for before in [False, True]:
                compare('counter-patch-' + str(reverse), path, combined, driver, 100, before, resources=[('buffer', 60)])
    from validate_output_commands import fixture as output_base
    source = output_base(a.out / 'joint-base.gpa_frame')
    x = Fixture(); x.records = []
    with Frame(source) as f:
        for en in f.entries.values(): x.add(en.id, en.category, en.type, f.payload(en.id))
    x.add(70, 7, 0x242, struct.pack('<QQ', 0, 1))
    path = x.write(a.out / 'joint.gpa_frame')
    for driver in ['hardware', 'warp']:
        for event in [82, 83]:
            compare('keep-rt', path, [setter(82, 0x3500, om([], 999999, 5, [224], [11], keep_rt=True))], driver, event)
        compare('keep-uav', path, [setter(84, 0x3500, om([44], 212, KEEP, keep_uav=True))], driver, 84)
    source = fixture(a.out / 'high-base.gpa_frame')
    x = Fixture(); x.records = []
    with Frame(source) as f:
        for en in f.entries.values():
            raw = f.payload(en.id); kind = en.type
            if en.id == 80: kind = 0x3500; raw = om([44], 0, 63, [224], [7])
            if en.id == 85: raw = cs(2, [0])
            if en.id == 99:
                raw = bytearray(raw)
                struct.pack_into('<I', raw, 17484, 63)
                struct.pack_into('<I', raw, 17528, 64)
                struct.pack_into('<Q', raw, 20976 + 55 * 8, 224)
            x.add(en.id, en.category, kind, raw)
    path = x.write(a.out / 'high-om.gpa_frame')
    ops = [setter(80, 0x3500, om([234], 0, 63, [224], [11]))]
    for driver in ['hardware', 'warp']:
        compare('high-om-baseline', path, [], driver, resources=[('texture', 30), ('buffer', 220)])
        compare('high-om-edit', path, ops, driver, resources=[('texture', 230), ('buffer', 220)])
        patches = [operation(220, 0, struct.pack('<I', 99)), dict(kind='uav_counter', event=100, view=224, value=5)]
        for reverse in [False, True]:
            compare('high-om-patches-' + str(reverse), path, patches + ops if reverse else ops + patches, driver, resources=[('buffer', 220)])
    for stage in ['vs', 'hs', 'ds', 'gs', 'ps', 'cs']:
        path = collateral_fixture(a.out / ('mixed-' + stage + '.gpa_frame'), stage)
        ops = [setter(88, 0x25e, cs(1, [0]))]
        for driver in ['hardware', 'warp']:
            compare('mixed-' + stage, path, ops, driver, resources=[('buffer', 74)] if stage == 'cs' else [('texture', 30)])
            compare('mixed-reset-' + stage, path, ops, driver, 200)
            if stage == 'ps':
                patch = dict(kind='setter', event=90, values=dict(start_slot=2, views=[902, 922]))
                for reverse in [False, True]:
                    compare('mixed-input-' + str(reverse), path, [patch] + ops if reverse else ops + [patch], driver, resources=[('texture', 30)])

if not a.quick:
    source = fixture(a.out / 'gap-base.gpa_frame')
    for case in ['unknown-prefix', 'missing-output', 'retained-so', 'draw-auto']:
        x = Fixture(); x.records = []
        with Frame(source) as f:
            for en in f.entries.values():
                if case == 'unknown-prefix' and en.id == 70: continue
                raw = f.payload(en.id); kind = en.type
                if case in ('retained-so', 'draw-auto') and en.id == 80:
                    raw = rt([254], 0) if case == 'retained-so' else rt([], 0)
                if case == 'draw-auto' and en.id in (99, 199):
                    raw = bytearray(raw); struct.pack_into('<Q', raw, 13964, 250); struct.pack_into('<I', raw, 14028, 1)
                if case == 'draw-auto' and en.id == 200: kind = 0x38; raw = raw[:24]
                x.add(en.id, en.category, kind, raw)
        if case == 'missing-output': x.add(87, 7, 0x34ff, rt([999999], 0))
        if case in ('retained-so', 'draw-auto'):
            x.add(75, 7, 0x3503, struct.pack('<QQI', 0, 1, 1) + array('Q', [250]) + array('I', [0]))
        path = x.write(a.out / (case + '.gpa_frame'))
        value = rt([254], 0) if case == 'draw-auto' else rt([], 0) if case == 'retained-so' else rt([234], 0)
        for driver in ['hardware', 'warp']:
            compare(case, path, [setter(80, 0x34ff, value)], driver, 200 if case == 'draw-auto' else 100,
                    fail=case != 'retained-so')
    path = ia_fixture(a.out / 'geometry-ia.gpa_frame')
    for driver in ['hardware', 'warp']:
        geometry('geometry-ia', path, [setter(85, 0x25e, cs(0, [0, 0]))], driver)

if a.captures:
    from output_commands import TYPES as OUTPUT_TYPES
    for name, minimum in [('GF2_Exilium_2026_03_03__00_19_35.gpa_frame', 0), ('bf1_2026_01_21__16_53_05.gpa_frame', 27696)]:
        path = a.captures / name
        with Frame(path) as f:
            candidate = chosen = None
            for en in sorted((v for v in f.entries.values() if v.category == 7), key=lambda v: v.id):
                if en.type == 0x34ff and en.id > minimum:
                    values = captured(en.type, f.payload(en.id))
                    if values['rtvs'] and values['rtvs'][0]: candidate = (en, values)
                elif en.type in OUTPUT_TYPES: candidate = None
                elif en.type in DRAW_TYPES and candidate: chosen = (*candidate, en)
            assert chosen, name
            command, values, draw = chosen
            values = {**values, 'rtvs': [0] * len(values['rtvs'])}
        for driver in ['hardware', 'warp']:
            compare(name + '-edited', path, [dict(kind='setter', event=command.id, values=values)], driver, draw.id)

save(completed=True)
print('PASS', len(checks), 'checks', flush=True)
