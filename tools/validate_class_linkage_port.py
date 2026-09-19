"""Development-only comparison of dynamic class replay with the preserved Python implementation."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--qt-bin', type=Path)
p.add_argument('--isolated-env', action='store_true')
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference.resolve()), str(a.reference.resolve().parent / 'tools')]
from validate_class_linkage import fixture, clone, COMMON, POSITIONS
from validate_buffer_edits import experiment
from frame import Frame
from devices import create_device
from engine import Engine
from experiments import Experiment, blob
from class_linkage import describe
from shaders import compile_hlsl, resource_metadata
from replay_pipeline import inspect

env = dict(os.environ)
if a.isolated_env:
    env = {k: v for k, v in env.items() if k.upper() in {'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA'}}
    env['PATH'] = str(Path(os.environ['WINDIR']) / 'System32') + os.pathsep + os.environ['WINDIR']
if a.qt_bin:
    env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + env['PATH']
checks = []


def check(name, passed, **detail):
    checks.append(dict(case=name, passed=bool(passed), **detail))
    (a.out / 'validation.json').write_text(json.dumps(checks, indent=2) + '\n')
    assert passed, checks[-1]
    print(name, 'PASS', flush=True)


def native(name, path, command, args=(), driver='hardware', project=None, failure=False):
    out = a.out / name
    cmd = [str(a.exe.resolve()), command, str(path.resolve()), '--out', str(out.resolve()), *args]
    if driver == 'warp':
        cmd += ['--warp']
    if project:
        cmd += ['--experiment', str(project.resolve())]
    result = subprocess.run(cmd, env=env, capture_output=True, timeout=90)
    (a.out / (name + '.log')).write_bytes(result.stderr)
    if failure:
        # An access violation is not an explicit rejection.
        assert result.returncode == 1, (name, result.returncode, result.stderr)
        assert b'"completed":false' in result.stderr, (name, result.stderr)
        return result.stderr.decode(errors='replace')
    assert result.returncode == 0, (name, result.stderr.decode(errors='replace'))
    report = json.loads((out / 'report.json').read_text())
    assert report['reference_pixels_used'] is False
    if command in {'replay', 'buffer', 'replay-pipeline'}:
        assert report['completed']
        assert not any(Path(m).name.lower().startswith(('python', 'gpa_', 'gpa-', 'tk8', 'tcl8')) for m in report['loaded_modules'])
    return out


def compare(name, path, stage, driver='hardware', project=None, event=100, pipeline=False, before=False):
    with Frame(path) as frame:
        device = create_device(driver)
        try:
            engine = Engine(frame, device, experiment=Experiment(frame, project) if project else None)
            if pipeline:
                expected = inspect(engine, event, after=not before)
            else:
                engine.replay(until=event, before=before, readback=False)
                expected = engine.read_buffer(74) if stage == 'cs' else engine.readback(30)[2]
        finally:
            device.close()
    args = ['--event', str(event)] + (['--before'] if before else [])
    if pipeline:
        out = native(name, path, 'replay-pipeline', args, driver, project)
        actual = json.loads((out / 'replay-pipeline.json').read_text())
        # Bookkeeping counts are implementation-specific; bindings and provenance are exact.
        keys = ('event', 'api', 'context', 'value_time', 'source', 'experiment_applied', 'fields', 'command_enabled', 'known_fields', 'unknown_fields', 'notes', 'limits')
        differences = {k: dict(native=actual.get(k), python=expected[k]) for k in keys if actual.get(k) != expected[k]}
        if differences:
            (a.out / (name + '-diff.json')).write_text(json.dumps(differences, indent=2))
        check(name, not differences, fields=len(actual['fields']))
    else:
        command = 'buffer' if stage == 'cs' else 'replay'
        out = native(name, path, command, args + ['--id', '74' if stage == 'cs' else '30'], driver, project)
        actual = (out / ('buffer.bin' if stage == 'cs' else 'frame.rgba')).read_bytes()
        check(name, actual == expected, bytes=len(actual))


paths = {}
for stage in ('ps', 'vs', 'gs', 'cs', 'hs', 'ds'):
    for created, second, index in ((False, False, 0), (False, True, 0), (False, False, 1), (True, False, 0), (True, True, 0)):
        key = f'{stage}-{int(created)}-{int(second)}-{index}'
        path = a.out / (key + '.gpa_frame')
        info = fixture(path, stage, created, second, index)
        paths[key] = path
        for driver in ('hardware', 'warp'):
            compare(key + '-' + driver, path, stage, driver)
            if not second and index == 0:
                compare(key + '-' + driver + '-pipeline', path, stage, driver, pipeline=True)
        with Frame(path) as frame:
            out = native(key + '-instance', path, 'class-linkage', ['--id', '62'])
            check(key + '-instance', json.loads((out / 'class-linkage.json').read_text()) == describe(frame, 62))
            out = native(key + '-shader', path, 'shader', ['--id', str(info['shader'])])
            meta = json.loads((out / 'shader.json').read_text())
            check(key + '-shader', meta['interface_slots'] == 1 and meta['class_linkage_id'] == 60 and
                  (out / 'shader.dxbc').read_bytes() == frame.shader(info['shader'] + 1))

# A static replacement removes interfaces; a dynamic replacement retains the captured linkage.
path = paths['ps-0-0-0']
with Frame(path) as frame:
    for label, source in [('dynamic', COMMON.replace('return x+value;', 'return x+value.bgra;') + 'float4 main():SV_Target{return selected.apply(0);}'),
                          ('static', 'float4 main():SV_Target{return float4(0,0,1,1);}')]:
        asset = a.out / (label + '.dxbc')
        asset.write_bytes(compile_hlsl(source, 'ps_5_0')[0])
        project = a.out / (label + '.json')
        experiment(frame, project, [dict(kind='shader', resource=12, asset=blob(asset))])
        for driver in ('hardware', 'warp'):
            compare(label + '-' + driver, path, 'ps', driver, project)
            compare(label + '-' + driver + '-pipeline', path, 'ps', driver, project, pipeline=True)

# Ordered interface arrays, including two slots referring to distinct named instances.
multi = a.out / 'multiple-base.gpa_frame'
fixture(multi, source=COMMON.replace('ITransform selected;', 'ITransform selected[2];') +
        'float4 main():SV_Target{return selected[0].apply(0)*.75+selected[1].apply(0)*.25;}')
for reverse in (False, True):
    def change(en, raw):
        if en.id in (99, 199):
            raw = bytearray(raw)
            struct.pack_into('<2Q', raw, 15328, *((66, 62) if reverse else (62, 66)))
            struct.pack_into('<I', raw, 17376, 2)
        return raw
    def extra(x):
        x.add(66, 5, 0x98, struct.pack('<QQ8IQ', 0, 60, 0, 0, 0, 0, 0, 0, 0, 0, 68))
        x.add(68, 9, 0x89, struct.pack('<I', 7) + b'second\0' + bytes(4))
    path = clone(multi, a.out / f'multiple-{reverse}.gpa_frame', change, extra)
    for driver in ('hardware', 'warp'):
        compare(f'multiple-{reverse}-{driver}', path, 'ps', driver)
        compare(f'multiple-{reverse}-{driver}-pipeline', path, 'ps', driver, pipeline=True)

# Preserve the reference's captured shader-setter behavior outside active edits:
# draw snapshots prepare the program and interfaces; dormant setters do not.
for stage, kind in dict(vs=0x34e9, hs=0x351a, ds=0x351e, gs=0x34f5, ps=0x34e7, cs=0x3523).items():
    shader = 12 if stage == 'ps' else 10 if stage == 'vs' else 80
    def extra(x):
        x.add(90, 7, kind, struct.pack('<QQQIBQ', 0, 1, shader, 1, 1, 62))
        x.add(95, 7, kind, struct.pack('<QQQIB', 0, 1, 0, 0, 0))
    path = clone(paths[f'{stage}-0-0-0'], a.out / f'{stage}-setters.gpa_frame', lambda en, raw: raw, extra)
    for event in (90, 95, 100):
        for before in (True, False):
            compare(f'{stage}-setter-{event}-{before}', path, stage, pipeline=True, event=event, before=before)

# Dynamic texture/sampler offsets must survive SRV pruning. The one-class variant
# also checks the reference's specific NVIDIA driver guard without rewriting DXBC.
texture_source = '''interface ITransform{float4 apply(float4 x);};class A:ITransform{Texture2D<float4> tex;SamplerState samp;float4 apply(float4 x){return tex.SampleLevel(samp,float2(.5,.5),0);}};class B:ITransform{Texture2D<float4> tex;SamplerState samp;float4 apply(float4 x){return tex.SampleLevel(samp,float2(.5,.5),0)*.5;}};A first;B second;ITransform selected;float4 main():SV_Target{return selected.apply(0);}'''
texture = a.out / 'texture-base.gpa_frame'
fixture(texture, created=True, source=texture_source)
def texture_change(en, raw):
    if en.id in (99, 199):
        raw = bytearray(raw)
        struct.pack_into('<Q', raw, 14304 + 4 * 8, 94)
        struct.pack_into('<Q', raw, 14168 + 5 * 8, 96)
    return raw
def texture_extra(x):
    x.add(90, 5, 0x85, bytes(16) + struct.pack('<11IQ', 8, 8, 1, 1, 28, 1, 0, 0, 8, 0, 0, 91))
    x.data(91, bytes([0, 0, 255, 255]) * 64)
    x.view(94, 'srv', 90, [28, 4, 0, 1, 0, 0])
    x.add(96, 5, 0x88, bytes(16) + struct.pack('<4IfII4f2f', 0, 3, 3, 3, 0., 1, 1, 0, 0, 0, 0, 0., 1000.))
path = clone(texture, a.out / 'texture.gpa_frame', texture_change, texture_extra)
for driver in ('hardware', 'warp'):
    compare('texture-' + driver, path, 'ps', driver)
    compare('texture-' + driver + '-pipeline', path, 'ps', driver, pipeline=True)
source = texture_source[:texture_source.index('class B:')] + texture_source[texture_source.index('A first;'):].replace('B second;', '')
code = compile_hlsl(source, 'ps_5_0')[0]
inline = clone(path, a.out / 'texture-inline.gpa_frame', lambda en, raw: struct.pack('<Q', len(code)) + code + bytes(8) if en.id == 13 else raw)
compare('texture-inline-warp', inline, 'ps', 'warp')
d = create_device('hardware')
try:
    adapter = d.adapter_info()
finally:
    d.close()
if (adapter['vendor_id'], adapter['device_id']) == (0x10de, 0x249d):
    message = native('texture-inline-guard', inline, 'replay', ['--event', '100'], failure=True)
    check('texture-inline-guard', '--warp' in message)
else:
    compare('texture-inline-hardware', inline, 'ps')

for label in ('count', 'null_instance', 'wrong_linkage', 'bad_name', 'bad_created'):
    def change(en, raw):
        raw = bytearray(raw)
        if label == 'count' and en.id in (99, 199): struct.pack_into('<I', raw, 17376, 2)
        if label == 'null_instance' and en.id in (99, 199): struct.pack_into('<Q', raw, 15328, 0)
        if label == 'wrong_linkage' and en.id == 62: struct.pack_into('<Q', raw, 8, 70)
        if label == 'bad_name' and en.id == 64: raw[9] = 65
        if label == 'bad_created' and en.id == 62: struct.pack_into('<I', raw, 44, 2)
        return raw
    path = clone(paths['ps-0-0-0'], a.out / (label + '.gpa_frame'), change)
    rejected = False
    with Frame(path) as frame:
        d = create_device('warp')
        try:
            try: Engine(frame, d).replay(until=100, readback=False)
            except (ValueError, RuntimeError): rejected = True
        finally: d.close()
    native(label, path, 'replay', ['--event', '100'], 'warp', failure=True)
    check(label, rejected)
print(len(checks), 'class linkage checks PASS', flush=True)
