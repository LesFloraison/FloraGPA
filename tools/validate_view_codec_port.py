"""Development oracle for resource-wide view descriptor wire and merge rules."""
import argparse
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
p.add_argument('--qt-bin', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--captures', type=Path)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
sys.path.insert(0, str(a.reference / 'standalone'))
import view_edits as ref
from frame import Frame

cases, expected = [], []
def add(kind, action, **values):
    case = dict(kind=kind, action=action, **values)
    cases.append(case)
    try:
        if action == 'normalize': value = ref.normalize(kind, values['value'])
        elif action == 'keys': value = list(ref.keys(kind, values['dimension']))
        elif action == 'pack': value = ref.pack(kind, values['value']).hex()
        elif action == 'unpack': value = ref.unpack(kind, bytes.fromhex(values['bytes']))
        elif action == 'merge': value = ref.merge(kind, values['previous'], values['value'])
        else: raise AssertionError(action)
        expected.append(dict(status='ok', value=value))
    except (ValueError, KeyError, TypeError, struct.error) as e:
        expected.append(dict(status='error', error=str(e)))

for kind, dimensions in ref.FIELDS.items():
    for dim in range(14): add(kind, 'keys', dimension=dim)
    for invalid in [None, [], {}, 3, False, 'bad', {'unknown': 1}]:
        add(kind, 'normalize', value=invalid)
    for flag in range(8): add(kind, 'normalize', value={'flags': flag})
    for dim in dimensions:
        keys = ref.keys(kind, dim)
        value = {key: dim if key == 'dimension' else 28 if key == 'format' else 0 if key == 'flags' else 1 for key in keys}
        add(kind, 'normalize', value=value)
        add(kind, 'pack', value=value)
        packed = ref.pack(kind, value)
        add(kind, 'unpack', bytes=packed.hex())
        for size in range(len(packed)):
            add(kind, 'unpack', bytes=packed[:size].hex())
        add(kind, 'unpack', bytes=(packed+b'\0').hex())
        words = list(struct.unpack('<' + 'I' * (len(packed) // 4), packed))
        for index in range(len(keys), len(words)): words[index] = 0xdeadbeef
        add(kind, 'unpack', bytes=struct.pack('<' + 'I' * len(words), *words).hex())
        for key in keys:
            for changed in [0, 1, 0xffffffff, 0x100000000, -1, True, False, 1.0, '1', None, [], {}]:
                add(kind, 'normalize', value={key: changed})
                add(kind, 'merge', previous=value, value={key: changed})
                add(kind, 'pack', value={**value, key: changed})
            incomplete = dict(value); del incomplete[key]
            add(kind, 'pack', value=incomplete)
        for next_dim in dimensions:
            add(kind, 'merge', previous=value, value={'dimension': next_dim})
            target = {key: next_dim if key == 'dimension' else 41 if key == 'format' else 0 if key == 'flags' else 2 for key in ref.keys(kind, next_dim)}
            add(kind, 'merge', previous=value, value=target)
        for key in ['flags', 'first_element', 'mip_slice', 'first_w_slice', 'num_cubes', 'most_detailed_mip']:
            add(kind, 'merge', previous=value, value={key: 1})

capture_counts = {}
if a.captures:
    for name in ['GF2_Exilium_2026_03_03__00_19_35.gpa_frame', 'bf1_2026_01_21__16_53_05.gpa_frame']:
        count = 0
        with Frame(a.captures / name) as frame:
            for entry in frame.entries.values():
                if entry.category != 5 or entry.type not in ref.KINDS: continue
                info = ref.describe(frame, entry.id)
                add(info['kind'], 'unpack', bytes=frame.payload(entry.id)[24:].hex())
                add(info['kind'], 'pack', value=info['descriptor'])
                add(info['kind'], 'merge', previous=info['descriptor'], value={'format': info['descriptor']['format']})
                count += 1
        capture_counts[name] = count

request = a.out / 'request.json'
request.write_text(json.dumps(dict(cases=cases)))
env = {k:v for k,v in os.environ.items() if k.upper() in {'SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}}
env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + os.environ['WINDIR'] + '/System32;' + os.environ['WINDIR']
run = subprocess.run([str(a.exe.resolve()), '--oracle', str(request.resolve())], env=env, capture_output=True, timeout=180)
(a.out / 'native.stdout.json').write_bytes(run.stdout)
(a.out / 'native.stderr.txt').write_bytes(run.stderr)
assert run.returncode == 0, run.stderr
actual = json.loads(run.stdout)
assert len(actual) == len(expected)
failures = []
for index, (left, right) in enumerate(zip(expected, actual, strict=True)):
    if left['status'] != right['status'] or left['status'] == 'ok' and left['value'] != right.get('value'):
        failures.append(dict(index=index, case=cases[index], expected=left, actual=right))
report = dict(completed=True, passed=not failures, cases=len(cases),
              accepted=sum(v['status']=='ok' for v in expected),
              expected_rejections=sum(v['status']=='error' for v in expected),
              captures=capture_counts, failures=failures,
              executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),
              reference_sha256=hashlib.sha256(Path(ref.__file__).read_bytes()).hexdigest())
(a.out / 'validation.json').write_text(json.dumps(report, indent=2))
assert not failures, failures[:3]
print('PASS', len(cases), 'view codec comparisons; captures', capture_counts)
