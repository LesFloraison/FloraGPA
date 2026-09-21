"""Development-only comparison with the original inventory/texture worker."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
for key in ('reference', 'exe', 'qt-bin', 'captures', 'history', 'out'):
    p.add_argument('--' + key, type=Path, required=True)
p.add_argument('--cases', nargs='+', help='Run only named comparisons')
p.add_argument('--empty-capture', type=Path, help='Optional action-free native recapture')
p.add_argument('--serializer', type=Path, help='Native asset serializer probe executable')
a = p.parse_args()
a.out = a.out.resolve()
a.out.mkdir(parents=True, exist_ok=False)
env = dict(os.environ)
env['PATH'] = str(a.qt_bin.resolve()) + ';C:/Windows/System32;C:/Windows'
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
checks = []
totals = dict(comparisons=0, resources=0, textures=0, buffers=0, texture_bytes=0)


def save(complete=False):
    (a.out / 'validation.json').write_text(json.dumps(dict(
        complete=complete, passed=complete and all(c['passed'] for c in checks),
        **totals, checks=checks), indent=2), 'utf-8')


def check(name, ok, **details):
    checks.append(dict(name=name, passed=bool(ok), **details))
    save()
    print(name, 'PASS' if ok else 'FAIL', flush=True)
    assert ok, name


def compare(label, capture, action, **selection):
    options = dict(action=action, capture=str(capture.resolve()), gpa_event=None, eid=None,
                   resource=None, x=0, y=0, mip=0, layer=0, sample=0)
    options.update(selection)
    native_out = a.out / (label + '-native')
    job = a.out / (label + '-native-job.json')
    job.write_text(json.dumps(dict(options, out=str(native_out),
                                  renderdoc='C:/Program Files/RenderDoc/renderdoc.dll')), 'utf-8')
    native = subprocess.run([str(a.exe.resolve()), '--job', str(job)], env=env,
                            capture_output=True, timeout=180)
    (a.out / (label + '-native.log')).write_bytes(native.stdout + native.stderr)
    actual = json.loads((native_out / 'result.json').read_text('utf-8'))
    check(label + '-native', native.returncode == 0 and actual['ok'], error=actual.get('error'))
    oracle_out = a.out / (label + '-oracle')
    oracle_out.mkdir()
    oracle_job = a.out / (label + '-oracle-job.json')
    oracle_job.write_text(json.dumps(dict(options, out=str(oracle_out))), 'utf-8')
    script = a.out / (label + '-oracle.py')
    script.write_text('import sys\nsys.path.insert(0,' + repr(str(a.reference / 'standalone')) +
                      ')\nfrom rdc_worker import run\nrun(' + repr(str(oracle_job)) +
                      ')\nsys.exit(0)\n', 'utf-8')
    oracle = subprocess.run(['C:/Program Files/RenderDoc/qrenderdoc.exe', '--python', str(script)],
                            startupinfo=startup, capture_output=True, timeout=180)
    (a.out / (label + '-oracle.log')).write_bytes(oracle.stdout + oracle.stderr)
    expected = json.loads((oracle_out / 'result.json').read_text('utf-8'))
    check(label + '-oracle', oracle.returncode == 0 and expected['ok'], error=expected.get('error'))
    fields = ['action', 'eid', 'gpa_event', 'gpa_event_map', 'gpa_command_map']
    fields += (['resources', 'textures', 'buffers', 'actions'] if action == 'inventory' else
               ['resource', 'resource_name', 'byte_length', 'subresource'])
    different = [key for key in fields if actual.get(key) != expected.get(key)]
    check(label + '-exact-metadata', not different, fields=different)
    if action == 'inventory':
        for field in ('resources', 'textures', 'buffers'):
            totals[field] += len(actual[field])
    else:
        raw = (native_out / 'texture.bin').read_bytes()
        reference = (oracle_out / 'texture.bin').read_bytes()
        check(label + '-exact-bytes', raw == reference and len(raw) == actual['byte_length'],
              bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(),
              oracle_sha256=hashlib.sha256(reference).hexdigest())
        totals['texture_bytes'] += len(raw)
    modules = [Path(s).name.lower() for s in actual['loaded_modules']]
    check(label + '-runtime', not any(m.startswith(('python', 'gpa_', 'gpa-', 'tk8', 'tcl8')) or
          m in ('gpa.dll', 'dx11_player.dll', 'dx11_playback.dll', 'shimd3d64.dll') for m in modules))
    totals['comparisons'] += 1
    save()


if a.serializer:
    target = a.out / 'annotations-native.json'
    run = subprocess.run([str(a.serializer.resolve()), '--probe', str(target)], env=env,
                         capture_output=True, timeout=30)
    check('annotation-native', run.returncode == 0)
    oracle = a.out / 'annotations-oracle.json'
    script = a.out / 'annotations-oracle.py'
    script.write_text(
        'import sys,json,renderdoc as rd\nsys.path.insert(0,' + repr(str(a.reference / 'standalone')) +
        ')\nfrom rdc_worker import plain\nrows=[]\n'
        'for bits in [0,123,255,0x7ff0000000000000,0xfff0000000000000,0x7ff8000000000000,0x8000000000000000,2**64-1]:\n'
        ' obj=rd.SDObject("probe","uint64_t");obj.type.basetype=rd.SDBasic.UnsignedInteger;obj.data.basic.u=bits\n'
        ' rows.append(plain(obj))\n'
        'open(' + repr(str(oracle)) + ',"w").write(json.dumps(rows,indent=2))\nsys.exit(0)\n', 'utf-8')
    run = subprocess.run(['C:/Program Files/RenderDoc/qrenderdoc.exe', '--python', str(script)],
                         startupinfo=startup, capture_output=True, timeout=30)
    check('annotation-exact', run.returncode == 0 and
          json.loads(target.read_text('utf-8')) == json.loads(oracle.read_text('utf-8')), comparisons=8)

cases = []
if a.empty_capture:
    cases.append(('empty-inventory', a.empty_capture, 'inventory', {}))
for name in ('commands', 'commands-warp', 'msaa', 'msaa-warp', 'msaa-disabled',
             'msaa-before', 'msaa-测试', 'gf2', 'bf1'):
    capture = a.captures / (name + '-rdc') / 'independent_capture.rdc'
    cases.append((name + '-inventory', capture, 'inventory', {}))
for name in ('array', 'uint', 'sint', 'volume', 'line', 'partial', 'uav-clear'):
    cases.append((name + '-inventory', a.history / (name + '-recapture') / 'independent_capture.rdc',
                  'inventory', {}))
for name in ('msaa', 'msaa-warp', 'gf2', 'bf1'):
    cases.append((name + '-target', a.captures / (name + '-rdc') / 'independent_capture.rdc',
                  'texture', {}))
for sample in range(4):
    cases.append(('msaa-sample-' + str(sample), a.captures / 'msaa-rdc/independent_capture.rdc',
                  'texture', dict(gpa_event=18, sample=sample)))
for name in ('array', 'array-other-mip', 'uint', 'sint', 'volume', 'line', 'partial',
             'uav-clear', 'update-api'):
    original = json.loads((a.history / (name + '-native-job.json')).read_text('utf-8'))
    selected = {k: original[k] for k in ('gpa_event', 'eid', 'resource', 'mip', 'layer', 'sample')}
    cases.append((name + '-raw', Path(original['capture']), 'texture', selected))
if a.cases:
    assert set(a.cases) <= {case[0] for case in cases}, 'Unknown case name'
for label, capture, action, selection in cases:
    if not a.cases or label in a.cases:
        compare(label, capture, action, **selection)
save(complete=True)
