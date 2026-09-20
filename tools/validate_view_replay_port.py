"""Development-only global view experiments compared with the preserved Python engine."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--qt-bin', type=Path)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--captures', type=Path)
p.add_argument('--real-only', action='store_true')
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference.resolve() / 'standalone'), str(a.reference.resolve() / 'tools')]
from frame import Frame
from dx11 import Device
from engine import Engine
from validate_buffer_edits import experiment, Fixture, state_bytes
from validate_srv_descriptors import dimension_fixture, extra_fixture
from validate_texture_outputs import fixture
from replay_pipeline import inspect
from view_edits import describe

env = {k: v for k, v in os.environ.items() if k.upper() in
       {'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA'}}
env['PATH'] = os.environ['WINDIR'] + '/System32;' + os.environ['WINDIR']
if a.qt_bin:
    env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + env['PATH']
checks = []

def check(name, passed, **details):
    checks.append(dict(name=name, passed=bool(passed), **details))
    (a.out / 'validation.json').write_text(json.dumps(dict(
        passed=all(c['passed'] for c in checks), checks=checks,
        executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest()), indent=2))
    print(name, 'PASS' if passed else 'FAIL', flush=True)
    assert passed, name

def native(name, capture, action, driver, project, event, resource=None, fail=False):
    out = a.out / name
    cmd = [str(a.exe.resolve()), action, str(capture.resolve()), '--out', str(out.resolve()),
           '--experiment', str(project.resolve())]
    if event:
        cmd += ['--event', str(event)]
    if resource is not None:
        cmd += ['--id', str(resource)]
    if driver == 'warp':
        cmd += ['--warp']
    r = subprocess.run(cmd, env=env, capture_output=True, timeout=180)
    (a.out / (name + '.log')).write_bytes(r.stdout + r.stderr)
    if fail:
        check(name + '-rejected', r.returncode == 1 and b'"completed":false' in r.stderr)
        return
    if r.returncode:
        raise RuntimeError((name, r.returncode, r.stderr.decode(errors='replace')[-3000:]))
    report = json.loads((out / 'report.json').read_text())
    assert report['completed'] and report['reference_pixels_used'] is False
    assert not any(Path(m).name.lower().startswith(('python', 'gpa_', 'gpa-', 'tk8', 'tcl8')) or
                   Path(m).name.lower() == 'renderdoc.dll' for m in report['loaded_modules'])
    return out

def compare(name, path, ops, driver, event=100, resource=30, texture=False, field='cs.srv.0'):
    project = a.out / (name + '-' + driver + '.json')
    with Frame(path) as frame:
        exp = experiment(frame, project, ops)
        d = Device(driver)
        try:
            engine = Engine(frame, d, experiment=exp)
            engine.replay(event, readback=False)
            expected = engine.read_texture(resource) if texture else engine.read_buffer(resource)
            state = inspect(Engine(frame, d, experiment=exp), event) if field else None
        finally:
            d.close()
    prefix = name + '-' + driver
    out = native(prefix + '-storage', path, 'texture-storage' if texture else 'buffer', driver,
                 project, event, resource)
    actual = (out / ('texture.bin' if texture else 'buffer.bin')).read_bytes()
    (a.out / (prefix + '-expected.bin')).write_bytes(expected)
    check(prefix + '-storage', actual == expected, size=len(expected),
          expected=hashlib.sha256(expected).hexdigest(), actual=hashlib.sha256(actual).hexdigest())
    if not field:
        return
    out = native(prefix + '-pipeline', path, 'replay-pipeline', driver, project, event)
    actual = json.loads((out / 'replay-pipeline.json').read_text())
    wanted = next(row for row in state['fields'] if row['field'] == field)
    observed = next(row for row in actual['fields'] if row['field'] == field)
    for key in ('descriptor', 'descriptor_source'):
        check(prefix + '-' + key, observed['object'].get(key) == wanted['object'].get(key),
              expected=wanted['object'].get(key), actual=observed['object'].get(key))

def view(id, values):
    return dict(kind='view', resource=id, values=values)

if not a.real_only:
    for dim in (1, 2, 3, 4, 5, 8, 9, 10, 11):
        path, patch, _ = dimension_fixture(a.out / f'srv-{dim}.gpa_frame', dim)
        for driver in ('warp', 'hardware'):
            compare(f'srv-{dim}', path, [view(22, patch)], driver)
            compare(f'srv-{dim}-original', path, [], driver)
    for kind in ('structured', 'format', 'ms', 'msarray'):
        path, patch = extra_fixture(a.out / f'srv-{kind}.gpa_frame', kind)
        for driver in ('warp', 'hardware'):
            compare(f'srv-{kind}', path, [view(22, patch)], driver)
    path, _, _ = dimension_fixture(a.out / 'srv-composed.gpa_frame', 4)
    global_edit = view(22, dict(most_detailed_mip=1, mip_levels=1))
    local_edit = dict(kind='srv_descriptor', event=100, stage='cs', slot=0, values=dict(mip_levels=1))
    for order, ops in enumerate(([global_edit, local_edit], [local_edit, global_edit])):
        for driver in ('warp', 'hardware'):
            compare(f'srv-composed-{order}', path, ops, driver)
    for role in ('rtv', 'dsv', 'uav', 'om_uav'):
        for dim in ('1d', '2d', '3d'):
            if role == 'dsv' and dim == '3d':
                continue
            name = role + '-' + dim
            path = a.out / (name + '.gpa_frame')
            fixture(path, dim, role, 40 if role == 'dsv' else 42)
            patch = dict(first_w_slice=1, w_size=1) if dim == '3d' else dict(first_array_slice=0, array_size=1)
            field = {'rtv': 'rtv.0', 'dsv': 'dsv', 'uav': 'cs.uav.0', 'om_uav': 'om.uav.1'}[role]
            for driver in ('warp', 'hardware'):
                for suffix, ops in [('', [view(30, patch)]), ('-original', [])]:
                    compare(name + suffix, path, ops, driver, resource=20, texture=True, field=field)

    for kind in ('typed', 'raw', 'structured'):
        x = Fixture()
        flags = 1 if kind == 'raw' else 0
        fmt = 39 if kind == 'raw' else 0 if kind == 'structured' else 42
        x.buffer(20, [32, 0, 128, 0, 32 if kind == 'raw' else 64 if kind == 'structured' else 0,
                      4 if kind == 'structured' else 0], struct.pack('<8I', *([7] * 8)))
        x.view(30, 'uav', 20, [fmt, 1, 0, 8, flags])
        typ = 'RWByteAddressBuffer' if kind == 'raw' else 'RWStructuredBuffer<uint>' if kind == 'structured' else 'RWBuffer<uint>'
        body = 'dst.Store(0,99);' if kind == 'raw' else 'dst[0]=99;'
        x.shader(10, 'cs', typ + ' dst:register(u0);[numthreads(1,1,1)]void main(){' + body + '}')
        x.events(state_bytes([(17772, 10), (20864, 30)]), 0x35, struct.pack('<3I', 1, 1, 1))
        path = x.write(a.out / ('buffer-' + kind + '.gpa_frame'))
        for driver in ('warp', 'hardware'):
            compare('buffer-' + kind, path, [view(30, dict(first_element=4, num_elements=4))], driver,
                    resource=20, field='cs.uav.0')
    from validate_uav_counters import fixture as counter_fixture
    for kind in ('append', 'counter', 'consume'):
        source = counter_fixture(a.out / ('counter-' + kind + '-source.gpa_frame'), kind)
        x = Fixture()
        x.records = []
        with Frame(source) as frame:
            for entry in frame.entries.values():
                raw = frame.payload(entry.id)
                if entry.category == 7 and entry.type == 0x3f:
                    # The legacy synthetic fixture omitted this command's owner.
                    # Both engines receive the same explicit immediate context.
                    raw = bytearray(raw)
                    struct.pack_into('<Q', raw, 8, 1)
                x.add(entry.id, entry.category, entry.type, raw)
        path = x.write(a.out / ('counter-' + kind + '.gpa_frame'))
        for driver in ('warp', 'hardware'):
            for resource in (28, 30, 32):
                compare('counter-' + kind + '-' + str(resource), path,
                        [view(46, dict(first_element=2, num_elements=6))], driver, event=101,
                        resource=resource, field=None)
    for fmt, flags in ((40, 1), (45, 1), (45, 2), (45, 3)):
        path = a.out / f'depth-{fmt}-{flags}.gpa_frame'
        fixture(path, '2d', 'dsv', fmt)
        for driver in ('warp', 'hardware'):
            compare(f'depth-{fmt}-{flags}', path, [view(30, dict(flags=flags))], driver,
                    texture=True, resource=20, field='dsv')
    x = Fixture()
    x.add(20, 5, 0x85, bytes(16) + struct.pack('<11IQ', 4, 4, 2, 1, 27, 1, 0, 0, 40, 0, 0, 21))
    x.data(21, bytes(80))
    x.view(30, 'rtv', 20, [28, 4, 0, 0, 0])
    x.add(100, 7, 0x32, struct.pack('<QQQB4f', 0, 1, 30, 1, .5, 0, 0, 1))
    path = x.write(a.out / 'clear-format.gpa_frame')
    for driver in ('warp', 'hardware'):
        compare('clear-format-original', path, [], driver, texture=True, resource=20, field=None)
        compare('clear-format', path, [view(30, dict(format=29, mip_slice=1))], driver,
                texture=True, resource=20, field=None)
        for index, patch in enumerate((dict(format=40), dict(mip_slice=2))):
            project = a.out / f'invalid-{driver}-{index}.json'
            with Frame(path) as frame:
                exp = experiment(frame, project, [view(30, patch)])
                d = Device(driver)
                try:
                    try:
                        Engine(frame, d, experiment=exp).replay(100, readback=False)
                        rejected = False
                    except RuntimeError:
                        rejected = True
                finally:
                    d.close()
            check(f'invalid-{driver}-{index}-reference', rejected)
            native(f'invalid-{driver}-{index}', path, 'texture-storage', driver, project, 100, 20, fail=True)
    x = Fixture()
    x.buffer(20, [32, 0, 32, 0, 0, 0], struct.pack('<8I', *([7] * 8)))
    x.view(30, 'rtv', 20, [42, 1, 0, 8, 0])
    x.add(100, 7, 0x32, struct.pack('<QQQB4f', 0, 1, 30, 1, 1, 0, 0, 1))
    path = x.write(a.out / 'buffer-rtv.gpa_frame')
    for driver in ('warp', 'hardware'):
        compare('buffer-rtv', path, [view(30, dict(first_element=2, num_elements=3))], driver,
                resource=20, field=None)
    x = Fixture()
    x.add(20, 5, 0x85, bytes(16) + struct.pack('<11IQ', 4, 4, 3, 1, 28, 1, 0, 0, 40, 0, 1, 21))
    x.data(21, bytes([255, 0, 0, 255]) * 16 + bytes([0, 255, 0, 255]) * 4 + bytes(4))
    x.view(30, 'srv', 20, [28, 4, 0, 3, 0, 0])
    x.add(100, 7, 0x245, struct.pack('<QQQ', 0, 1, 30))
    path = x.write(a.out / 'generate-mips.gpa_frame')
    for driver in ('warp', 'hardware'):
        compare('generate-mips', path, [view(30, dict(most_detailed_mip=1, mip_levels=2))], driver,
                texture=True, resource=20, field=None)

if a.captures:
    for filename in ('GF2_Exilium_2026_03_03__00_19_35.gpa_frame', 'bf1_2026_01_21__16_53_05.gpa_frame'):
        path = a.captures / filename
        with Frame(path) as frame:
            srvs, rtvs = [], []
            for entry in frame.entries.values():
                if entry.category != 5 or entry.type not in (0x8c, 0x8d):
                    continue
                desc = describe(frame, entry.id)['descriptor']
                if entry.type == 0x8c and desc.get('mip_levels', 0) not in (0, 1, 0xffffffff):
                    srvs.append(view(entry.id, dict(most_detailed_mip=desc['most_detailed_mip'] + 1,
                                                   mip_levels=desc['mip_levels'] - 1)))
                elif entry.type == 0x8d and desc['format'] == 29:
                    rtvs.append(view(entry.id, dict(format=28)))
            assert srvs and rtvs
            for variant, ops in [('srv', srvs), ('rtv', rtvs), ('original', [])]:
                name = path.stem + '-' + variant
                project = a.out / (name + '.json')
                exp = experiment(frame, project, ops)
                d = Device('hardware')
                try:
                    _, _, expected = Engine(frame, d, experiment=exp).replay()
                finally:
                    d.close()
                out = native(name, path, 'replay', 'hardware', project, None)
                actual = (out / 'frame.rgba').read_bytes()
                check(name, actual == expected, edits=len(ops), expected=hashlib.sha256(expected).hexdigest(),
                      actual=hashlib.sha256(actual).hexdigest())
