"""Development-only argument/history oracle for native output and SO edits."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
from types import SimpleNamespace

p = argparse.ArgumentParser()
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--qt-bin', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--frames', type=Path)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference / 'standalone'), str(a.reference / 'tools')]
from frame import Frame
from binding_model import FIELDS
from command_state import snapshot_values
from state import decode_state
from events import DRAW_TYPES
from output_setters import History, encode as encode_output
from output_commands import TYPES as OUTPUTS, KEEP, MissingOutputResource
from setter_edits import captured, validate
from so_setters import encode as encode_so
from stream_output import SET_TARGET_TYPES
from validate_output_setters import fixture as output_fixture, collateral_fixture, ia_fixture
from validate_so_setters import fixture as so_fixture
from validate_output_commands import rt, cs, om, array
from validate_buffer_edits import Fixture

env = dict(os.environ)
env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + str(Path(os.environ['WINDIR']) / 'System32')
checks = []

def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()

def save(completed=False):
    (a.out / 'validation.json').write_text(json.dumps(dict(completed=completed, passed=completed,
                                                        checks=checks, cases=sum(c['cases'] for c in checks)), indent=2))

def native(label, path, request):
    request['frame'] = str(path.resolve())
    file = a.out / (label + '.request.json')
    file.write_text(json.dumps(request), encoding='utf-8')
    result = subprocess.run([str(a.exe.resolve()), '--oracle', str(file.resolve())], env=env, capture_output=True, timeout=300)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace'))
    return json.loads(result.stdout)

def compare(label, expected, actual):
    for i, (left, right) in enumerate(zip(expected, actual)):
        if left != right:
            (a.out / (label + '.mismatch.json')).write_text(json.dumps(dict(index=i, expected=left, actual=right), indent=2))
            raise AssertionError((label, i))
    assert len(expected) == len(actual)
    checks.append(dict(name=label, cases=len(expected), accepted=sum(row['status'] == 'ok' for row in expected),
                       rejected=sum(row['status'] != 'ok' for row in expected), sha256=digest(actual)))
    save()
    print(label, len(expected), 'PASS', flush=True)

def arguments(label, path, steps):
    expected = []
    with Frame(path) as f:
        for step in steps:
            event = f.entries[step['event']]
            try:
                values = validate(f, event, step['values'])
                encoded = encode_output(event.type, f.payload(event.id), values) if event.type in OUTPUTS else encode_so(f.payload(event.id), values)
                row = dict(status='ok', captured=captured(event.type, f.payload(event.id)), encoded=encoded.hex())
            except (ValueError, KeyError, TypeError, struct.error, IndexError, OverflowError):
                row = dict(status='error')
            expected.append(row)
    actual = native(label, path, dict(mode='arguments', steps=steps))
    actual = [{k: v for k, v in row.items() if k != 'error'} if row['status'] == 'ok' else dict(status='error') for row in actual]
    compare(label, expected, actual)

def history(label, path, edits, events, state=True):
    request = dict(mode='history', edits=[dict(event=e, replacement=raw.hex()) for e, raw in edits.items()], steps=[])
    expected = []
    with Frame(path) as f:
        exp = SimpleNamespace(events={e: dict(setter=captured(f.entries[e].type, raw)) for e, raw in edits.items()})
        h = History(f, exp)
        for event in events:
            step = dict(event=event)
            empty = decode_state(bytes(22320))
            en = f.entries.get(event)
            if en and en.category == 7 and en.type in DRAW_TYPES:
                empty = decode_state(f.payload(struct.unpack_from('<Q', f.payload(event))[0]))
            if state:
                step.update(state={k: v for k, v in snapshot_values(f, empty).items() if k in FIELDS}, so_offsets=empty['so_offsets'])
            row = {}
            try:
                row['delta'] = h.delta(event)
                if state:
                    s = h.state(event, empty)
                    row.update(state=digest({k: v for k, v in snapshot_values(f, s).items() if k in FIELDS}),
                               so_offsets=list(s['so_offsets']), so_retained_slots=sorted(s.get('so_retained_slots', ())))
                row['status'] = 'ok'
            except MissingOutputResource as error:
                row.update(status='missing', missing=error.resources)
            except (ValueError, KeyError, TypeError, struct.error, IndexError):
                row['status'] = 'error'
            row['gaps'] = copy.deepcopy({str(k): v for k, v in h.gaps.items()})
            expected.append(row)
            request['steps'].append(step)
    actual = native(label, path, request)
    actual = [{k: v for k, v in row.items() if k != 'error'} for row in actual]
    compare(label, expected, actual)

base = output_fixture(a.out / 'outputs-base.gpa_frame')
x = Fixture(); x.records = []
with Frame(base) as f:
    for en in f.entries.values(): x.add(en.id, en.category, en.type, f.payload(en.id))
for event, kind, raw in [(4000, 0x3500, om([44], 0, 3, [224], [KEEP])), (4001, 0x3522, cs(63, [224], [7]))]:
    x.add(event, 7, kind, raw)
path = x.write(a.out / 'arguments.gpa_frame')
steps = []
with Frame(path) as f:
    for event in [80, 85, 4000, 4001]:
        original = captured(f.entries[event].type, f.payload(event))
        steps.append(dict(event=event, values=original))
        for key, value in original.items():
            candidates = [None, True, -1, 1.5, '0', 2**64, {}, [], [0], [0, 0], [True], [2**64]]
            if isinstance(value, int): candidates += [0, 8, 64, KEEP]
            for candidate in candidates:
                steps.append(dict(event=event, values={**original, key: candidate}))
            missing = dict(original); del missing[key]
            steps.append(dict(event=event, values=missing))
        steps.append(dict(event=event, values={**original, 'extra': 0}))
        for candidate in [None, [], True]: steps.append(dict(event=event, values=candidate))
    for event, kind, raw in [(80, 0x34ff, rt([], 0)), (4000, 0x3500, om([], 999999, KEEP, keep_rt=True, keep_uav=True)),
                             (4000, 0x3500, om([], 0, 63, [224], [0], keep_rt=True)), (4000, 0x3500, om([234], 212, KEEP, keep_uav=True))]:
        steps.append(dict(event=event, values=captured(kind, raw)))
arguments('output-arguments', path, steps)

for n in range(5):
    path = so_fixture(a.out / ('so-' + str(n) + '.gpa_frame'), n)
    good = [dict(count=1, buffers=[40], offsets=[64]), dict(count=1, buffers=[42], offsets=None),
            dict(count=1, buffers=[40], offsets=[KEEP]), dict(count=0, buffers=None, offsets=None),
            dict(count=0, buffers=[], offsets=[]), dict(count=1, buffers=[0], offsets=[KEEP])]
    steps = [dict(event=120, values=v) for v in good]
    for key in ['count', 'buffers', 'offsets']:
        for value in [True, -1, 5, 1.5, None, [], [0], [40, 40], [0, 0], [44], [999999], [3], [516], [2**64]]:
            steps.append(dict(event=120, values={**good[0], key: value}))
    steps += [dict(event=120, values=dict(count=2, buffers=[40, 40], offsets=[0, KEEP])),
              dict(event=120, values=dict(count=2, buffers=[40, 42], offsets=[0, 512]))]
    arguments('so-arguments-' + str(n), path, steps)
    for index, value in enumerate(good):
        with Frame(path) as f: encoded = encode_so(f.payload(120), value)
        history('so-history-' + str(n) + '-' + str(index), path, {120: encoded}, [120, 150, 175, 180, 200, 150, 120, 0])

path = base
for name, edits in [('none', {}), ('move-uav', {85: cs(63, [224], [11])}), ('rtv', {80: rt([234], 0)}),
                    ('no-op', {80: rt([44], 0)}), ('both', {80: rt([234], 0), 85: cs(63, [224], [11])})]:
    history('output-history-' + name, path, edits, [80, 85, 100, 120, 200, 100, 80, 99, 0])

for stage in ['vs', 'hs', 'ds', 'gs', 'ps', 'cs']:
    path = collateral_fixture(a.out / ('collateral-' + stage + '.gpa_frame'), stage)
    with Frame(path) as f:
        events = sorted(e.id for e in f.entries.values() if e.category == 7)
        output = next(e for e in f.entries.values() if e.category == 7 and e.type in (0x3522, 0x25e))
    history('collateral-' + stage, path, {output.id: cs(0, [0])}, events + events[:3])
    srv_edit = struct.pack('<QQII', 0, 1, 2, 2) + array('Q', [912, 902])
    history('collateral-mixed-' + stage, path, {output.id: cs(0, [0]), 90: srv_edit}, events + events[:3])
path = ia_fixture(a.out / 'ia.gpa_frame')
history('ia-collateral', path, {85: cs(0, [0, 0])}, [80, 85, 90, 91, 100, 200, 91, 100])

for name in ['contexts', 'future-invalid']:
    x = Fixture(); x.records = []
    with Frame(base) as f:
        for en in f.entries.values(): x.add(en.id, en.category, en.type, f.payload(en.id))
    if name == 'contexts':
        x.add(50000, 5, 0x127, bytes(24))
        x.add(72, 7, 0x242, struct.pack('<QQ', 0, 50000))
        raw = bytearray(cs(0, [202])); struct.pack_into('<Q', raw, 8, 50000)
        x.add(86, 7, 0x25e, raw)
        x.add(89, 7, 0x37, struct.pack('<QQQII', 99, 0, 50000, 0, 0))
        path = x.write(a.out / (name + '.gpa_frame'))
        replacement = bytearray(cs(63, [224])); struct.pack_into('<Q', replacement, 8, 50000)
        history(name, path, {85: cs(0, [202]), 86: bytes(replacement)}, [80, 85, 86, 89, 100, 120, 200, 86])
    else:
        x.add(5001, 7, 0x3522, bytes(18))
        path = x.write(a.out / (name + '.gpa_frame'))
        history(name, path, {85: cs(63, [224])}, [100, 200, 5001, 5001, 100, 200])

for name, gap, edits in [
    ('unknown-view', cs(0, [999999]), {}),
    ('edited-unknown-view', cs(0, [999999]), {85: cs(63, [224])}),
    ('missing-counter-reset', cs(0, [999999], [0]), {}),
    ('known-counter-reset', cs(0, [999999, 224], [KEEP, 0]), {}),
    ('irrelevant-count', cs(0, [999999, 202], [KEEP, 7]), {}),
    ('keep-count', cs(0, [999999, 224], [KEEP, KEEP]), {}),
    ('replace-missing', cs(0, [999999]), {87: cs(0, [224])}),
]:
    x = Fixture(); x.records = []
    with Frame(base) as f:
        for en in f.entries.values(): x.add(en.id, en.category, en.type, f.payload(en.id))
    x.add(87, 7, 0x25e, gap)
    x.add(89, 7, 0x242, struct.pack('<QQ', 0, 1))
    path = x.write(a.out / (name + '.gpa_frame'))
    history('gap-' + name, path, edits, [85, 87, 87, 89, 100, 200, 85, 87])

if a.frames:
    for name in ['GF2_Exilium_2026_03_03__00_19_35.gpa_frame', 'bf1_2026_01_21__16_53_05.gpa_frame']:
        path = a.frames / name
        with Frame(path) as f:
            events = [e.id for e in sorted(f.entries.values(), key=lambda x: x.id) if e.category == 7 and e.type in DRAW_TYPES]
        history(name.split('_')[0] + '-capture', path, {}, events + events[:3])
save(completed=True)
print('PASS', len(checks), 'groups', sum(c['cases'] for c in checks), 'cases', flush=True)
