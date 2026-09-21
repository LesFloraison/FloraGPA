"""Compare native GPA/RDC event provenance to the recovered Python controller."""
import argparse
import json
import os
from pathlib import Path
import random
import subprocess
import sys
from types import SimpleNamespace as NS

p = argparse.ArgumentParser(description=__doc__)
for name in ('reference', 'exe', 'qt-bin', 'out'):
    p.add_argument('--' + name, type=Path, required=True)
p.add_argument('--real', type=Path, nargs='*', default=[])
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
sys.path.insert(0, str(a.reference / 'standalone'))
from rdc_events import index, provenance

class Flags:
    Clear = 1
    Drawcall = 2
    Dispatch = 4
    PushMarker = 0x40
    Copy = 0x400
    Resolve = 0x800
    GenMips = 0x1000

class Bits(int):
    def __new__(cls, value, label):
        result = int.__new__(cls, value)
        result.label = label
        return result
    def __str__(self):
        return self.label

def converted(n):
    return NS(**dict(n, flags=Bits(n['flags'], n['flags_text']),
                     events=[NS(**x) for x in n['events']],
                     children=[converted(x) for x in n['children']]))

def expected(case):
    actions, _, selected, reverse, commands = index(NS(ActionFlags=Flags),
        [converted(x) for x in case['roots']], {int(k): v for k, v in case.get('native', {}).items()})
    return json.loads(json.dumps(dict(actions=actions, gpa_event_map=selected,
        gpa_command_map=commands, reverse=reverse,
        provenance={eid: provenance(eid, reverse) for eid in case.get('lookup', [])})))

def node(eid, name='', flags=0, children=None, events=None):
    return dict(eventId=eid, customName=name, flags=flags, flags_text=str(flags),
                numIndices=3, numInstances=2, children=children or [], events=events or [])

rng = random.Random(73145)
cases = []
names = ['Draw', 'DrawAuto', 'DrawIndexed', 'DrawInstanced', 'DrawIndexedInstanced',
         'DrawInstancedIndirect', 'DrawIndexedInstancedIndirect', 'Dispatch', 'DispatchIndirect']
api = ['ClearDepthStencilView', 'ClearRenderTargetView', 'ClearUnorderedAccessViewUint',
       'ClearUnorderedAccessViewFloat', 'CopyResource', 'CopySubresourceRegion', 'CopyStructureCount',
       'ResolveSubresource', 'GenerateMips', 'UpdateSubresource', 'MapCapturedWrites']
for i in range(700):
    roots, native = [], {}
    for j in range(rng.randint(1, 12)):
        eid = j * 20 + 1
        command = rng.choice(names + api + ['Unknown', 'DrawInvalid'])
        is_api = rng.choice([True, False])
        gpa = rng.choice([1, 2, 10, 20, 50, 18446744073709551615])
        name = 'GPA {}{}: {}'.format('API ' if is_api else '', gpa, command)
        if i % 7 == 0:
            name += rng.choice(['\n', ' extra', ' ', '🌱'])
        children = []
        for k in range(rng.randint(0, 4)):
            event = eid + k * 3 + 1
            flags = rng.choice([0, 1, 2, 4, 0x400, 0x800, 0x1000, 0x102])
            name2 = rng.choice(['', 'helper', 'GPA broken', 'ordinary group', 'GPA 700: Draw'])
            raw = [dict(eventId=event - 1), dict(eventId=event)]
            native[str(event)] = dict(name=rng.choice(['ID3D11DeviceContext::Map',
                'ID3D11DeviceContext::Unmap', 'ID3D11DeviceContext::UpdateSubresource', 'helper']))
            children.append(node(event, name2, flags, [node(event + 1, '', flags)] if k == 0 else [], raw))
        roots.append(node(eid, name, rng.choice([0x40, 0x40, 0x40, 2, 4]), children))
    cases.append(dict(roots=roots, native=native, lookup=list(range(1, len(roots) * 20 + 1))))
for path in a.real:
    case = json.loads(path.read_text(encoding='utf-8'))
    case['lookup'] = sorted({int(k) for k in case['native']})
    cases.append(case)
source = a.out / 'cases.json'
source.write_text(json.dumps(cases, ensure_ascii=False), encoding='utf-8')
env = dict(os.environ)
env['PATH'] = str(a.qt_bin) + os.pathsep + env['PATH']
output = a.out / 'native.json'
subprocess.run([str(a.exe.resolve()), '--index', str(source.resolve()), str(output.resolve())],
               check=True, env=env, timeout=120)
actual = json.loads(output.read_text(encoding='utf-8'))
checks = []
for i, (case, result) in enumerate(zip(cases, actual, strict=True)):
    want = expected(case)
    adapter = 'reference' not in case or all(case['reference'][k] == want[k] for k in case['reference'])
    checks.append(dict(case=i, passed=adapter and result['ok'] and result['result'] == want,
                       oracle_adapter_exact=adapter))
report = dict(passed=all(x['passed'] for x in checks), count=len(checks), real_captures=len(a.real), checks=checks)
(a.out / 'validation.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps({k: v for k, v in report.items() if k != 'checks'}), flush=True)
raise SystemExit(0 if report['passed'] else 1)
