"""Development oracle for the native dual binding model; no runtime Python dependency."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--qt-bin', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--frames', type=Path)
p.add_argument('--gpu', action='store_true')
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference / 'standalone'), str(a.reference / 'tools')]
from frame import Frame
from binding_model import BindingModel, FIELDS, TYPES
from command_state import snapshot_values
from output_commands import TYPES as OUTPUT_TYPES, MissingOutputResource, read
from stream_output import SET_TARGET_TYPES, targets
from srv_setters import TYPES as SRV_TYPES, captured as srv_values
from ia_setters import captured as ia_values
from validate_binding_model import fixture, snapshot, observe, native
from validate_output_commands import cs, rt, om, KEEP, array

checks = []
env = dict(os.environ)
env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + str(Path(os.environ['WINDIR']) / 'System32')

def record_results(completed=False):
    report = dict(completed=completed, passed=completed and all(c['passed'] for c in checks),
                  scope='C++/Python model parity; GPU differences recorded separately',
                  checks=checks, boundaries=sum(c['boundaries'] for c in checks))
    (a.out / 'validation.json').write_text(json.dumps(report, indent=2))

def digest(values):
    return hashlib.sha256(json.dumps(values, sort_keys=True, separators=(',', ':')).encode()).hexdigest()

def replacement(kind, raw):
    if kind in OUTPUT_TYPES:
        return read(kind, raw)
    if kind in SET_TARGET_TYPES:
        return targets(raw)
    if kind in SRV_TYPES:
        return srv_values(raw)
    return ia_values(kind, raw)

def run(label, frame, steps, context=1, device=None):
    model = BindingModel(frame, context)
    expected = []
    native_discrepancies = []
    engine = None
    if device:
        from engine import Engine
        engine = Engine(frame, device)
    for step in steps:
        row = {}
        try:
            if 'anchor' in step:
                model.anchor(snapshot(step['anchor']))
            elif step.get('gap'):
                row['gap'] = model.gap(frame.entries[step['event']])
            else:
                entry = frame.entries[step['event']]
                raw = bytes.fromhex(step['replacement']) if 'replacement' in step else None
                model.step(entry, replacement(entry.type, raw) if raw is not None else None)
                if engine:
                    native(engine, entry.id, raw)
            row['status'] = 'ok'
            if engine:
                actual = observe(engine)
                diff = {k: [model.changed[k], actual[k]] for k in FIELDS if model.changed[k] != actual[k]}
                if diff:
                    # Reproduced on hardware and WARP: ClearState leaves stride/
                    # offset metadata for a VB already nulled by an output hazard.
                    # Keep these differences visible; this is not exact GPU parity.
                    assert all(k.startswith(('strides.', 'offsets.')) and left == 0 and
                               model.changed['vb.' + k.split('.')[1]] == actual['vb.' + k.split('.')[1]] == 0
                               for k, (left, right) in diff.items()), (label, step, diff)
                    native_discrepancies.append(dict(boundary=len(expected), step=step, differences=diff))
        except MissingOutputResource as error:
            row.update(status='missing', missing=error.resources)
        except (ValueError, KeyError, struct.error, IndexError) as error:
            if engine:
                raise
            row.update(status='error', error=str(error))
        row.update(original=digest(model.original), changed=digest(model.changed), dirty=sorted(model.dirty))
        try:
            values = snapshot_values(frame, model.overlay(snapshot({k: 0 for k in FIELDS})))
            row['overlay'] = digest({k: values[k] for k in FIELDS})
        except ValueError:
            row['overlay'] = None
        expected.append(row)
    request = a.out / (label + '.request.json')
    request.write_text(json.dumps(dict(frame=str(frame.path.resolve()), context=context, steps=steps)), encoding='utf-8')
    process = subprocess.run([str(a.exe.resolve()), '--oracle', str(request.resolve())], env=env, capture_output=True, timeout=240)
    if process.returncode:
        raise RuntimeError(process.stderr.decode(errors='replace'))
    actual = json.loads(process.stdout)
    assert len(actual) == len(expected)
    for i, (left, right) in enumerate(zip(expected, actual)):
        left = {k: v for k, v in left.items() if k != 'error'}
        right = {k: v for k, v in right.items() if k != 'error'}
        if left != right:
            (a.out / (label + '.mismatch.json')).write_text(json.dumps(dict(step=steps[i], index=i, expected=left, actual=right), indent=2))
            raise AssertionError((label, i, steps[i]))
    checks.append(dict(name=label, passed=True, boundaries=len(steps), fields=len(FIELDS), gpu=bool(device),
                       native_exact=not native_discrepancies if device else None,
                       native_discrepancies=native_discrepancies, sha256=digest(actual)))
    record_results()
    print(label, len(steps), 'passed', flush=True)

path = fixture(a.out / 'bindings.gpa_frame')
with Frame(path) as frame:
    # Frame.path is not present in all versions of the reference reader.
    frame.path = path
    run('captured', frame, [{'event': e} for e in [300, *range(80, 99), *range(310, 326)]])
    scenarios = [
        ('disable-output', [300, 301, *range(310, 316), 320, 321, 302, 323, 300], 1, cs(0, [0])),
        ('add-output', [300, 302, *range(310, 316), 320, 321, 302, 323, 300], 1, cs(0, [202])),
        ('output-after-input', [300, *range(310, 316), 320, 321, 301, 302, 323, 300], 9, cs(0, [0])),
        ('move-output', [300, 301, 302, 323, 87, 97], 1, cs(63, [224])),
        ('change-rtv', [300, 80, 91, 84, 90], 1, rt([234], 212)),
        ('retain-so', [300, 324, 325, 90, 324, 300], 2, rt([], 0)),
        ('edit-so', [300, 324, 325, 90, 324, 300], 1, struct.pack('<QQI', 0, 1, 0) + array('Q', []) + array('I', [])),
    ]
    for name, sequence, index, raw in scenarios:
        original = BindingModel(frame, 1)
        steps = []
        for i, event in enumerate(sequence):
            original.step(frame.entries[event])
            steps.append(dict(event=event, **(dict(replacement=raw.hex()) if i == index else {})))
            steps.append(dict(anchor=dict(original.changed)))
        run(name + '-snapshots', frame, steps)
    for name, prefix in [('unedited', [dict(event=300)]), ('noop', [dict(event=300), dict(event=301, replacement=cs(0, [202]).hex())]),
                         ('edited', [dict(event=300), dict(event=302, replacement=cs(0, [202]).hex()), *[dict(event=x) for x in range(310, 316)], dict(event=320), dict(event=321)])]:
        steps = prefix + [dict(event=326), dict(event=326, gap=True), dict(event=310), dict(event=320), dict(anchor={k: 0 for k in FIELDS}), dict(event=300)]
        run('gap-' + name, frame, steps)
    invalid = [dict(event=301, replacement=cs(0, [0]).hex()), dict(event=300)]
    for raw in [cs(0, [999999]), cs(64, [202]), cs(0, [203]), cs(0, [202, 202]), cs(0, [202]) + b'\0', cs(0, [202])[:-1]]:
        invalid.append(dict(event=301, replacement=raw.hex()))
    run('invalid-atomic', frame, invalid)
    rng = random.Random(72591)
    for n in range(12):
        steps = [dict(event=300)]
        for i in range(80):
            event = rng.choice([*range(80, 99), *range(300, 303), *range(310, 327)])
            if event in (303, 304, 305, 306, 307, 308, 309, 316, 317, 318, 319):
                continue
            step = dict(event=event)
            kind = frame.entries[event].type
            if kind in OUTPUT_TYPES and rng.random() < .4:
                if kind == 0x34ff:
                    raw = rt(rng.choice([[], [44], [234], [254], [44, 234], [44, 44]]), rng.choice([0, 212]))
                elif kind == 0x3500:
                    raw = om([], 0, rng.choice([0, 3, 63]), [rng.choice([0, 202, 224])], keep_rt=True)
                else:
                    raw = cs(rng.choice([0, 3, 63]), [rng.choice([0, 202, 224])])
                step['replacement'] = raw.hex()
            steps.append(step)
            if event == 326:
                steps.append(dict(event=326, gap=True))
        run('mixed-' + str(n), frame, steps)
    if a.gpu:
        from dx11 import Device
        for driver in ('hardware', 'warp'):
            device = Device(driver)
            try:
                run(driver + '-captured-getters', frame, [dict(event=x) for x in [300, *range(80, 99)]], device=device)
            finally:
                device.close()
            for name, sequence, index, raw in scenarios:
                device = Device(driver)
                try:
                    original = BindingModel(frame, 1)
                    steps = []
                    for i, event in enumerate(sequence):
                        original.step(frame.entries[event])
                        steps.append(dict(event=event, **(dict(replacement=raw.hex()) if i == index else {})))
                        steps.append(dict(anchor=dict(original.changed)))
                    run(driver + '-' + name + '-getters', frame, steps, device=device)
                finally:
                    device.close()
if a.frames:
    from events import DRAW_TYPES
    for name in ['GF2_Exilium_2026_03_03__00_19_35.gpa_frame', 'bf1_2026_01_21__16_53_05.gpa_frame']:
        with Frame(a.frames / name) as frame:
            frame.path = a.frames / name
            by_context = {}
            for e in sorted(frame.entries.values(), key=lambda x: x.id):
                if e.category != 7 or e.type not in TYPES:
                    continue
                context = struct.unpack_from('<Q', frame.payload(e.id), 16 if e.type in DRAW_TYPES else 8)[0]
                by_context.setdefault(context, []).append(dict(event=e.id))
                if e.type in OUTPUT_TYPES:
                    v = read(e.type, frame.payload(e.id))
                    ids = (v.get('rtvs') or []) + (v.get('uavs') or []) + ([v.get('dsv', 0)] if v.get('rtv_count') != KEEP else [])
                    if any(x and x not in frame.entries for x in ids):
                        by_context[context].append(dict(event=e.id, gap=True))
            for context, steps in by_context.items():
                run(name.split('_')[0] + '-capture-' + str(context), frame, steps, context)
record_results(completed=True)
print('PASS', len(checks), 'streams', sum(c['boundaries'] for c in checks), 'boundaries', flush=True)
