"""Compare native texture edit storage rules with the preserved Python implementation.

Development-only: no Python dependency is introduced into FloraGPA.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--oracle', type=Path, required=True)
p.add_argument('--qt-bin', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
sys.path.insert(0, str(a.reference / 'standalone'))
from formats import texture_info, subresources
from msaa_edit import validate

cases = []
def resource(fmt=28, samples=1, kind=0x85, mips=1, layers=2, bind=40, misc=0):
    if kind == 0x84:
        desc = [8, mips, layers, fmt, 0, bind, 0, misc]
    elif kind == 0x86:
        desc = [8, 4, 4, mips, fmt, 0, bind, 0, misc]
    else:
        desc = [8, 4, mips, layers, fmt, samples, 0, 0, bind, 0, misc]
    return dict(type=kind, desc=desc)

def edit(r, **kwargs):
    item = dict(kind='edit', **r, mip=0, layer=1, sample=0 if texture_info(r)['samples'] > 1 else None)
    try:
        size = next(iter(subresources(texture_info(r))))['size']
    except ValueError:
        size = 32
    item['bytes'] = bytes(size).hex()
    item.update(kwargs)
    cases.append(item)
    return item

for fmt in range(117):
    for kind in (0x84, 0x85, 0x86, 0x87):
        for mips in (1, 3):
            cases.append(dict(kind='layout', **resource(fmt, kind=kind, mips=mips)))
    for bind in (8, 40, 64):
        r = resource(fmt, 4, bind=bind)
        for typed in (None, fmt, 0, 28, 42, 55):
            edit(r, typed_format=typed)
for fmt in (20, 41, 45, 26, 88):
    r = resource(fmt, 4, bind=64 if fmt in (20, 45) else 40)
    for sample in (None, -1, 0, 3, 4, 32, True, 1.0):
        edit(r, sample=sample)
    for layer in (-1, 0, 1, 2, True, 0.0):
        edit(r, layer=layer)
    for mip in (-1, 0, 1, True, 0.0):
        edit(r, mip=mip)
    for typed in (-1, True, '28', 28.0):
        edit(r, typed_format=typed)
    for count in (0, 1, 17):
        edit(r, bytes=bytes(count).hex())
for word in (0, 0x80000000, 1, 0x007fffff, 0x00800000, 0x3f800000, 0x3f800001, 0x7f800000, 0x7fc01234):
    edit(resource(20, 4, bind=64), bytes=(struct.pack('<II', word, 255) * 32).hex())
    edit(resource(41, 4), bytes=(struct.pack('<I', word) * 32).hex())
for padding in (0x100, 0x10000, 0x1000000):
    edit(resource(20, 4, bind=64), bytes=(struct.pack('<II', 0x3f000000, padding) * 32).hex())
for shift, bits in ((0, 6), (11, 6), (22, 5)):
    for mantissa in (0, 1, (1 << bits) - 1):
        edit(resource(26, 4), bytes=(struct.pack('<I', ((31 << bits) | mantissa) << shift) * 32).hex())
for x in (0, 1, 255):
    edit(resource(88, 4), bytes=(bytes([13, 67, 241, x]) * 32).hex())
for fmt in (28, 71, 104):
    for sample, typed in ((None, None), (0, None), (None, 0), (None, 28)):
        edit(resource(fmt), sample=sample, typed_format=typed)
for fmt in (103, 104, 105):
    for samples, mips, misc in ((1, 1, 0), (1, 2, 0), (4, 1, 0), (1, 1, 4)):
        cases.append(dict(kind='layout', **resource(fmt, samples, mips=mips, misc=misc)))

def expected(item):
    r = dict(type=item['type'], desc=item['desc'])
    info = texture_info(r)
    if item['kind'] == 'layout':
        return list(subresources(info))
    mip, layer = item['mip'], item['layer']
    if type(mip) is not int or type(layer) is not int:
        raise ValueError('Integer subresource required')
    sub = next((s for s in subresources(info) if s['mip'] == mip and s['layer'] == layer), None)
    data = bytes.fromhex(item['bytes'])
    if sub is None or len(data) != sub['size']:
        raise ValueError('Subresource mismatch')
    if info['samples'] == 1:
        if item.get('sample') is not None or item.get('typed_format') is not None:
            raise ValueError('Unexpected sample options')
        return None
    result = validate(r, layer, item.get('sample'), data, item.get('typed_format'))
    return json.loads(json.dumps(result))

reference = []
for item in cases:
    try:
        reference.append(dict(status='ok', value=expected(item)))
    except (ValueError, KeyError, TypeError) as e:
        reference.append(dict(status='error', error=str(e)))
request = a.out / 'request.json'
request.write_text(json.dumps(dict(cases=cases)))
env = os.environ.copy()
env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + env['PATH']
run = subprocess.run([str(a.oracle.resolve()), '--oracle', str(request.resolve())], env=env,
                     capture_output=True, timeout=120)
(a.out / 'native.log').write_bytes(run.stderr)
run.check_returncode()
actual = json.loads(run.stdout)
diffs = []
for index, (want, got) in enumerate(zip(reference, actual)):
    if want['status'] != got['status'] or (want['status'] == 'ok' and want['value'] != got.get('value')):
        diffs.append(dict(index=index, case=cases[index], expected=want, actual=got))
report = dict(completed=True, passed=len(actual) == len(reference) and not diffs,
              count=len(cases), accepted=sum(x['status'] == 'ok' for x in reference),
              rejected=sum(x['status'] == 'error' for x in reference), differences=diffs,
              executable_sha256=hashlib.sha256(a.oracle.read_bytes()).hexdigest())
(a.out / 'validation.json').write_text(json.dumps(report, indent=2))
print(json.dumps({k: v for k, v in report.items() if k != 'differences'}, indent=2))
if diffs:
    print(json.dumps(diffs[:3], indent=2))
raise SystemExit(0 if report['passed'] else 1)
