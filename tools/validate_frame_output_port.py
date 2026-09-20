"""Development-only Python/native display, view selection and presentation oracle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--oracle', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--qt-bin', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--cpu-only', action='store_true')
p.add_argument('--msaa-only', action='store_true')
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference / 'standalone'), str(a.reference / 'tools')]
from frame_output import display
from coverage_fragment import target_subresource
from presentation import inventory, parse_target, select
from formats import pitches
from frame import Frame
from validate_buffer_edits import Fixture, graphics_fixture, state_bytes
from validate_output_formats import fixture as format_fixture
from validate_output_subresources import fixture as layer_fixture
from dx11 import Device
from engine import Engine

env = {k: v for k, v in os.environ.items() if k.upper() in
       {'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA'}}
env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + os.environ['WINDIR'] + '/System32;' + os.environ['WINDIR']
cases, expected, checks = [], [], []
completed = False

def check(name, passed, **details):
    checks.append(dict(name=name, passed=bool(passed), **details))
    (a.out / 'validation.json').write_text(json.dumps(dict(
        completed=completed, passed=all(c['passed'] for c in checks), checks=checks,
        executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),
        oracle_sha256=hashlib.sha256(a.oracle.read_bytes()).hexdigest()), indent=2))
    print(name, 'PASS' if passed else 'FAIL', flush=True)
    assert passed, name

def add(action, **v):
    cases.append(dict(action=action, **v))
    try:
        if action == 'display':
            value = display(bytes.fromhex(v['bytes']), v['format'], v.get('channel', 'rgba'),
                            v.get('low', 0.), v.get('high', 1.), aspect=v.get('aspect')).hex()
        elif action == 'subresource':
            value = target_subresource(v['desc'], struct.pack('<'+'I'*len(v['view']), *v['view']),
                                       v.get('layer'), v.get('depth', False))
        elif action == 'target': value = parse_target(v['value'])
        elif action == 'inventory':
            with Frame(v['path']) as frame: value = inventory(frame)
        else: raise AssertionError(action)
        expected.append(dict(status='ok', value=value))
    except (ValueError, KeyError, TypeError, struct.error, OverflowError) as e:
        expected.append(dict(status='error', error=str(e)))

rng = random.Random(620260920)
for fmt in range(1, 116):
    try: size, _ = pitches(1, 1, fmt)
    except ValueError: continue
    data = bytes(size) + bytes([255])*size + rng.randbytes(size*24)
    for aspect in ([None, 'depth', 'stencil'] if fmt in (20, 40, 45, 55) else [None]):
        for channel in ('rgba', 'rgb', 'r', 'g', 'b', 'a'):
            for low, high in [(0, 1), (-1, 1), (0, 255), (-65536, 65536), (.25, .75)]:
                add('display', format=fmt, bytes=data.hex(), channel=channel, low=low, high=high,
                    **(dict(aspect=aspect) if aspect else {}))
    for data in [b'', b'\x01', bytes(size+1)]: add('display', format=fmt, bytes=data.hex())
for low, high in [(1, 1), (2, 1), (-1e308, 1e308)]:
    add('display', format=28, bytes='00000000', low=low, high=high)
for channel in ('', 'RGBA', 'red', 'rgbx'): add('display', format=28, bytes='', channel=channel)
for target in ['auto', 'present', 'depth', 'stencil', *('rt'+str(i) for i in range(10)),
               '', 'rt01', 'AUTO', 'swap:0', 'swap:01', 'swap:-1', 'swap:1',
               'swap:18446744073709551615', 'swap:18446744073709551616', 'swap:1junk']:
    add('target', value=target)
for kind, desc in [(0x83, [256, 0, 32, 0, 0, 0]),
                   (0x84, [8, 3, 4, 28, 0, 32, 0, 0]),
                   (0x85, [8, 4, 3, 4, 28, 1, 0, 0, 32, 0, 0]),
                   (0x85, [8, 4, 1, 4, 28, 4, 0, 0, 32, 0, 0]),
                   (0x86, [8, 4, 8, 3, 28, 0, 32, 0, 0])]:
    for depth in (False, True):
        for dimension in range(10):
            for fields in [(0, 0, 0), (0, 0, 1), (1, 1, 2), (1, 1, 0xffffffff), (3, 0, 1)]:
                view = [45 if depth else 28, dimension] + ([3] if depth else []) + list(fields)
                for layer in (None, 0, 1, 2, 4, 0xffffffff):
                    add('subresource', type=kind, desc=desc, view=view, depth=depth, layer=layer)

base = format_fixture(a.out / 'presentation.gpa_frame', 28, bytes([13, 41, 71, 255]))
with Frame(base) as frame:
    records = [(e.id, e.category, e.type, frame.payload(e.id)) for e in frame.entries.values()]
def variant(name, edit=lambda r: r, extra=()):
    f = Fixture(); f.records = []
    for record in records:
        changed = edit(record)
        if changed is not None: f.add(*changed)
    for record in extra: f.add(*record)
    path = f.write(a.out / (name+'.gpa_frame'))
    add('inventory', path=str(path.resolve()))
    return path
byid = {r[0]: r for r in records}
variants = [base]
add('inventory', path=str(base.resolve()))
variants.append(variant('missing-marker', lambda r: None if r[0] == 31 else r))
variants.append(variant('bad-marker', lambda r: (*r[:3], bytes(8)+r[3][8:]) if r[0] == 31 else r))
variants.append(variant('short-parent', lambda r: (*r[:3], r[3][:-1]) if r[0] == 30 else r))
variants.append(variant('mismatched-marker', lambda r: (*r[:3], r[3][:16]+struct.pack('<I', 99)+r[3][20:]) if r[0] == 31 else r))
variants.append(variant('duplicate-live', extra=[(60, *byid[20][1:])]))
variants.append(variant('duplicate-marker', extra=[(60, *byid[31][1:])]))
second_chain = [(70, 5, 0x38, bytes(88)),
                (71, 5, 0x85, byid[20][3][:8]+struct.pack('<Q', 70)+byid[20][3][16:60]+struct.pack('<Q', 11))]
variants.append(variant('second-chain', extra=second_chain))
variants.append(variant('poisoned-marker', lambda r: (*r[:3],r[3][:60]+struct.pack('<Q',61)) if r[0]==31 else r,
                        [(61,9,1,struct.pack('<I',32)+bytes([255,0,255,255])*8)]))
uninitialized = variant('uninitialized-backbuffer', extra=[(90,7,0x34f6,struct.pack('<QQI',0,1,4))])
request = a.out / 'request.json'
request.write_text(json.dumps(dict(cases=cases)))
run = subprocess.run([str(a.oracle.resolve()), '--oracle', str(request.resolve())], env=env,
                     capture_output=True, timeout=180)
(a.out / 'native.stdout.json').write_bytes(run.stdout)
(a.out / 'native.stderr.txt').write_bytes(run.stderr)
assert run.returncode == 0, run.stderr
actual = json.loads(run.stdout)
failures = [dict(index=i, case=cases[i], expected=l, actual=r)
            for i, (l, r) in enumerate(zip(expected, actual, strict=True))
            if l['status'] != r['status'] or l['status'] == 'ok' and l['value'] != r.get('value')]
(a.out / 'cpu-failures.json').write_text(json.dumps(failures, indent=2))
check('CPU display, target, subresource and inventory parity', not failures, cases=len(cases),
      accepted=sum(v['status']=='ok' for v in expected), rejections=sum(v['status']=='error' for v in expected))
if a.cpu_only:
    completed = True
    check('CPU suite completed', True)
    sys.exit(0)

def compare(name, path, driver, target='auto', event=None, layer=None, channel='rgba', low=0, high=1, sample=None):
    with Frame(path) as frame:
        d = Device(driver)
        try:
            engine = Engine(frame, d); engine.replay(event, readback=False)
            selection = select(engine, event, target)
            pixels = storage = meta = msaa = None
            if selection['resource'] is not None:
                _, _, pixels = engine.readback(selection['resource'], view=selection['view'],
                    aspect=selection.get('aspect'), layer=layer, channel=channel, low=low, high=high, sample=sample)
                storage, meta = engine.output_storage, engine.output_display
                msaa = engine.output_msaa
        finally: d.close()
    out = a.out / (name+'-'+driver)
    args = [str(a.exe.resolve()), 'replay', str(path.resolve()), '--out', str(out.resolve()),
            '--output-target', target, '--output-channel', channel, '--output-low', str(low), '--output-high', str(high)]
    if driver == 'warp': args += ['--warp']
    if event is not None: args += ['--event', str(event)]
    if layer is not None: args += ['--output-layer', str(layer)]
    if sample is not None: args += ['--output-sample', str(sample)]
    run = subprocess.run(args, env=env, capture_output=True, timeout=180)
    (a.out / (name+'-'+driver+'.log')).write_bytes(run.stdout+run.stderr)
    assert run.returncode == 0, (name, run.stderr)
    report = json.loads((out/'report.json').read_text())
    (out/'expected-selection.json').write_text(json.dumps(selection, indent=2))
    (out/'expected-display.json').write_text(json.dumps(meta, indent=2))
    check(name+'-'+driver+' selection', report['output_selection'] == selection,
          expected=selection, actual=report['output_selection'])
    check(name+'-'+driver+' availability', report['image_available'] == (pixels is not None))
    if pixels is not None:
        check(name+'-'+driver+' pixels/storage/metadata', (out/'frame.rgba').read_bytes() == pixels and
              (out/'output_storage.bin').read_bytes() == storage and report['output_display'] == meta)
        check(name+'-'+driver+' MSAA provenance', report.get('output_msaa') == msaa)
    else: check(name+'-'+driver+' no fabricated image', not (out/'frame.png').exists() and report['output_display'] is None)
    check(name+'-'+driver+' independent runtime', report['reference_pixels_used'] is False and not any(
        Path(m).name.lower().startswith(('python', 'gpa_', 'gpa-', 'tk8', 'tcl8')) or
        Path(m).name.lower() == 'renderdoc.dll' for m in report['loaded_modules']))

for path in ([] if a.msaa_only else variants):
    for driver in ('warp', 'hardware'):
        for target in ('auto', 'present', 'swap:30', 'rt7'):
            compare(path.stem+'-'+target.replace(':', '-'), path, driver, target)
        if path.stem == 'second-chain': compare('second-chain-explicit',path,driver,'swap:70')
if not a.msaa_only:
    for driver in ('warp', 'hardware'):
        compare('uninitialized-before-copy',uninitialized,driver,event=90)
        compare('initialized-after-copy',uninitialized,driver,event=100)
for name, fmt, raw in [('srgb',29,bytes([64,128,192,255])), ('bgra',87,bytes([192,128,64,255])),
                      ('bgrx',88,bytes([192,128,64,0])), ('rgb10a2',24,struct.pack('<I',0xe00803ff)),
                      ('fp16',10,struct.pack('<4e',.25,.5,2,1)), ('fp32',2,struct.pack('<4f',-.5,.5,2,1)),
                      ('uint',42,struct.pack('<I',16777217)), ('sint',43,struct.pack('<i',-42)),
                      ('r16',56,struct.pack('<H',32768)), ('rg8',49,bytes([64,128]))]:
    if a.msaa_only: break
    path = format_fixture(a.out/(name+'.gpa_frame'), fmt, raw)
    for driver in ('warp', 'hardware'):
        compare(name, path, driver)
        compare(name+'-range', path, driver, channel='b', high=4)
for name, kind, depth, all_w in [('1d',0x84,False,False), ('2d',0x85,False,False),
        ('3d',0x86,False,False), ('3d-all',0x86,False,True), ('1d-depth',0x84,True,False), ('2d-depth',0x85,True,False)]:
    if a.msaa_only: break
    path = layer_fixture(a.out/(name+'.gpa_frame'), kind, depth, all_w)
    for driver in ('warp', 'hardware'):
        for layer in (None, 1, 2):
            compare(name+'-'+str(layer), path, driver, 'depth' if depth else 'rt0', 100, layer, sample=0)
            if depth: compare(name+'-stencil-'+str(layer), path, driver, 'stencil', 100, layer, high=255)
path = graphics_fixture(a.out/'graphics.gpa_frame')
for driver in (() if a.msaa_only else ('warp', 'hardware')):
    for target in ('auto', 'rt0', 'rt7', 'depth', 'stencil', 'present'):
        compare('graphics-'+target, path, driver, target, 200)

from msaa import DEPTH, inspect_msaa
from validate_msaa import fill
def msaa_fixture(path, fmt):
    depth = DEPTH.get(fmt)
    f = Fixture()
    f.add(20, 5, 0x85, bytes(16)+struct.pack('<11IQ',7,5,1,2,fmt,4,0,0,64 if depth else 32,0,0,0))
    f.shader(10, 'vs', 'float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}')
    f.add(50,9,0x87,struct.pack('<I6f',1,0,0,7,5,0,1))
    f.add(52,5,0x89,bytes(16)+struct.pack('<IIIiffIIII',3,1,0,0,0.,0.,1,0,1,0))
    if depth:
        f.add(54,5,0x8b,bytes(16)+struct.pack('<13I',1,1,8,1 if depth[3] else 0,0xffff,1,1,3,8,1,1,3,8))
    for layer in range(2):
        vid = 22+layer
        if depth: f.add(vid,5,0x8e,bytes(16)+struct.pack('<Q6I',20,depth[0],6,0,layer,1,0))
        else: f.view(vid,'rtv',20,[fmt,7,layer,1,0])
        for sample in (range(4) if depth else [0]):
            shader = 200+layer*20+sample*2
            event = 1000+layer*100+sample*10
            if depth: code = f'float main():SV_Depth{{return {(sample+1)/8+layer/32};}}'
            elif fmt == 28: code = f'float4 main(uint s:SV_SampleIndex):SV_Target{{return float4((4+s*4+{layer*20})/255.0,0,0,1);}}'
            elif fmt == 3: code = f'uint4 main(uint s:SV_SampleIndex):SV_Target{{return uint4(4+s*4+{layer*20},16777217+s,0,1);}}'
            elif fmt == 4: code = f'int4 main(uint s:SV_SampleIndex):SV_Target{{return int4(-20+(int)s*4-{layer*20},0,0,1);}}'
            else: code = f'float4 main(uint s:SV_SampleIndex):SV_Target{{return float4((s+1)/8.0+{layer/32},0,0,1);}}'
            f.shader(shader,'ps',code)
            state = state_bytes([(908,10),(14048,50),(14040,52),(14296,shader),
                                 (17408,54 if depth else 0),(17488 if depth else 17420,vid)],
                                [(17528,0 if depth else 1),(17404,(1<<sample) if depth else 0xffffffff),
                                 (17416,7+sample*2+layer*16)])
            f.add(event-1,3,3,state)
            f.add(event,7,0x37,struct.pack('<3Q2I',event-1,0,1,3,0))
    return f.write(path)

for fmt in (28,3,4,41,44,39,19,53):
    path = msaa_fixture(a.out/f'msaa-pattern-{fmt}.gpa_frame',fmt)
    depth = DEPTH.get(fmt)
    for driver in ('warp','hardware'):
        for sample in (None,0,1,2,3):
            name = f'msaa-{fmt}-{sample}'
            compare(name,path,driver,'depth' if depth else 'rt0',sample=sample)
            raw = (a.out/(name+'-'+driver)/'output_storage.bin').read_bytes()
            d=Device(driver)
            try:
                r,obj=fill(d,fmt)
                _,full,_=inspect_msaa(d,obj,r,sample,depth[0] if depth else fmt)
            finally: d.close()
            check(name+'-'+driver+' independent sample-pattern oracle',raw==full[len(full)//2:])
            if depth and depth[3]:
                compare(name+'-stencil',path,driver,'stencil',high=255,sample=sample)
for fmt in (10,87,88,29,91):
    path=format_fixture(a.out/f'msaa-clear-{fmt}.gpa_frame',fmt,b'',4)
    for driver in ('warp','hardware'):
        for sample in (None,0,3): compare(f'msaa-clear-{fmt}-{sample}',path,driver,sample=sample)
completed = True
check('Requested output parity suite completed',True)
print(json.dumps(dict(completed=True, passed=True, checks=len(checks))), flush=True)
