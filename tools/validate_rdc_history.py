"""Development-only native RenderDoc replay/history versus the original Python worker."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
for key in ('reference', 'exe', 'cli', 'qt-bin', 'captures', 'out'):
    p.add_argument('--' + key, type=Path, required=True)
p.add_argument('--smoke', action='store_true')
a = p.parse_args()
a.out = a.out.resolve()
a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference / 'standalone'), str(a.reference / 'tools')]
from validate_mapped_textures import map_fixture
from validate_class_linkage import clone
from validate_command_resources import fixture
from validate_texture_outputs import texture

env = {k: v for k, v in os.environ.items() if k.upper() in
       ('SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'APPDATA', 'LOCALAPPDATA')}
env['PATH'] = str(a.qt_bin.resolve()) + ';' + os.environ['WINDIR'] + '/System32;' + os.environ['WINDIR']
dll = 'C:/Program Files/RenderDoc/renderdoc.dll'
oracle = 'C:/Program Files/RenderDoc/qrenderdoc.exe'
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
checks = []

def check(name, passed, **details):
    checks.append(dict(name=name, passed=bool(passed), **details))
    result = dict(passed=all(c['passed'] for c in checks), checks=checks,
                  executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest())
    (a.out / 'validation.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(name, 'PASS' if passed else 'FAIL', flush=True)
    if not passed:
        raise AssertionError(name)

def native(label, job, valid=True):
    job = dict(job, out=str(a.out / (label + '-native')), renderdoc=dll)
    source = a.out / (label + '-native-job.json')
    source.write_text(json.dumps(job), encoding='utf-8')
    run = subprocess.run([str(a.exe.resolve()), '--job', str(source)], env=env,
                         capture_output=True, timeout=180)
    (a.out / (label + '-native.log')).write_bytes(run.stdout + run.stderr)
    result = json.loads((Path(job['out']) / 'result.json').read_text(encoding='utf-8'))
    check(label + '-native-status', (run.returncode == 0) == valid and result['ok'] == valid)
    modules = [Path(x).name.lower() for x in result['loaded_modules']]
    check(label + '-native-runtime', not any(x.startswith(('python', 'tk8', 'tcl8', 'gpa_', 'gpa-')) or
          x in ('gpa.dll', 'dx11_player.dll', 'dx11_playback.dll', 'shimd3d64.dll') for x in modules))
    return result

def python_history(label, job):
    out = a.out / (label + '-oracle')
    out.mkdir()
    source = a.out / (label + '-oracle-job.json')
    source.write_text(json.dumps(dict(job, out=str(out))), encoding='utf-8')
    script = a.out / (label + '-oracle.py')
    script.write_text('import sys\nsys.path.insert(0,' + repr(str(a.reference / 'standalone')) + ')\n'
                      'from rdc_worker import run\nrun(' + repr(str(source)) + ')\nsys.exit(0)\n')
    run = subprocess.run([oracle, '--python', str(script)], startupinfo=startup,
                         capture_output=True, timeout=180)
    (a.out / (label + '-oracle.log')).write_bytes(run.stdout + run.stderr)
    return json.loads((out / 'result.json').read_text(encoding='utf-8'))

def history(label, capture, valid=True, **options):
    job = dict(action='history', capture=str(capture.resolve()), gpa_event=None, eid=None,
               resource=None, x=0, y=0, mip=0, layer=0, sample=0)
    job.update(options)
    actual = native(label, job, valid)
    want = python_history(label, job)
    check(label + '-oracle-status', want['ok'] == valid)
    if valid:
        fields = ('eid', 'gpa_event', 'gpa_event_map', 'gpa_command_map', 'resource', 'resource_name',
                  'x', 'y', 'history', 'history_events', 'history_scope')
        differences = [k for k in fields if actual.get(k) != want.get(k)]
        check(label + '-exact-history', not differences, differences=differences,
              records=actual['history_events'], cpu=actual['history_scope']['cpu_write_snapshots'])
    return actual

msaa = a.captures / 'msaa-rdc/independent_capture.rdc'
commands = a.captures / 'commands-rdc/independent_capture.rdc'
history('msaa', msaa, gpa_event=33, x=32, y=32)
if a.smoke:
    raise SystemExit(0)
history('msaa-sample', msaa, gpa_event=33, x=32, y=32, sample=1)
history('msaa-resolve', msaa, gpa_event=39, resource=40, x=8, y=8)
history('msaa-native-event', msaa, eid=47, x=8, y=8)
for label, opts in [('bad-mip', dict(mip=50)), ('bad-layer', dict(layer=50)),
                    ('bad-sample', dict(sample=50)), ('bad-x', dict(x=10000)),
                    ('bad-event', dict(gpa_event=999999))]:
    history(label, msaa, valid=False, **dict(dict(gpa_event=33), **opts))
for rid, mip, layer in [(84, 0, 0), (84, 1, 0), (84, 1, 1), (80, 1, 1),
                        (130, 1, 0), (100, 0, 1)]:
    history('commands-%s-%s-%s' % (rid, mip, layer), commands, gpa_event=300,
            resource=rid, mip=mip, layer=layer)
update = history('update-api', commands, gpa_event=213, resource=84, mip=1)
cpu = [r for r in update['history'] if r['record_kind'] == 'cpu_write_snapshot']
check('update-known-texels', len(cpu) == 1 and
      [round(v * 255) for v in cpu[0]['preMod']['col']['floatValue']] == [23] * 4 and
      [round(v * 255) for v in cpu[0]['postMod']['col']['floatValue']] == [64, 32, 16, 255])

def capture(label, source):
    out = a.out / (label + '-recapture')
    run = subprocess.run([str(a.cli.resolve()), 'replay', str(source.resolve()),
                          '--renderdoc', dll, '--out', str(out)], env=env,
                         capture_output=True, timeout=180)
    (a.out / (label + '-recapture.log')).write_bytes(run.stdout + run.stderr)
    check(label + '-recapture', run.returncode == 0)
    return Path(json.loads((out / 'report.json').read_text())['rdc_capture'])

seed = fixture(a.out / 'seed.gpa_frame')
def uav_extra(f):
    texture(f, 400, 0x85, [4,4,1,1,28,1,0,0,136,0,0], bytes(64))
    # SDK UAV descriptors have five uint32 fields; the old Python test supplied
    # a redundant sixth zero that its ctypes view constructor silently ignored.
    f.view(402, 'uav', 400, [28,4,0,0,0])
    f.add(250, 7, 0x34, struct.pack('<QQQB4f', 0, 1, 402, 1, .25, .5, .75, 1))
uav = capture('uav-clear', clone(seed, a.out / 'uav-clear.gpa_frame', lambda entry, raw: raw, uav_extra))
history('uav-clear', uav, gpa_event=300, resource=400)
def box_extra(f):
    f.data(92, bytes([64, 32, 16, 255]) * 2)
def box_edit(entry, raw):
    return raw[:16] + struct.pack('<QIB6IQII', 84, 1, 1, 1, 0, 0, 2, 2, 1, 92, 4, 8) if entry.id == 213 else raw
boxed = capture('boxed', clone(seed, a.out / 'boxed.gpa_frame', box_edit, box_extra))
history('box-inside', boxed, gpa_event=213, resource=84, mip=1, x=1, y=1)
history('box-outside', boxed, gpa_event=213, resource=84, mip=1, x=0, y=1)
for label, kw in [('array', {}), ('volume', dict(dimension='3d', index=1)),
                  ('line', dict(dimension='1d')), ('uint', dict(fmt=42)),
                  ('sint', dict(fmt=43)), ('partial', dict(dimension='1d', diff=True))]:
    source = a.out / (label + '.gpa_frame')
    meta = map_fixture(source, draw=True, **kw)
    rdc = capture(label, source)
    actual = history(label, rdc, gpa_event=200, resource=20, mip=1, layer=1)
    records = [r for r in actual['history'] if r['record_kind'] == 'cpu_write_snapshot']
    check(label + '-cpu-identity', len(records) == 1 and records[0]['gpa_event'] == 200 and
          records[0]['write']['map_type'] == 2 and actual['eid'] == records[0]['eventId'])
    offset = meta['sub']['offset']
    if label == 'volume':
        offset += meta['sub']['size'] // max(1, meta['info']['depth'] >> 1)
    before, after = meta['initial'][offset:offset + 4], meta['expected'][offset:offset + 4]
    if label in ('uint', 'sint'):
        field, code = ('uintValue', '<I') if label == 'uint' else ('intValue', '<i')
        check(label + '-known-values', records[0]['preMod']['col'][field][0] == struct.unpack(code, before)[0] and
              records[0]['postMod']['col'][field][0] == struct.unpack(code, after)[0])
    else:
        check(label + '-known-values', [round(v * 255) for v in records[0]['preMod']['col']['floatValue']] == list(before) and
              [round(v * 255) for v in records[0]['postMod']['col']['floatValue']] == list(after))
    if label == 'partial':
        unchanged = history('partial-unchanged', rdc, gpa_event=200, resource=20, mip=1, layer=1, x=2)
        check('partial-unchanged-honest', not unchanged['history'][0]['pixel_value_changed'])
    if label == 'array':
        history('array-other-mip', rdc, gpa_event=200, resource=20, mip=0, layer=1)

for dump in sorted(a.captures.glob('*-events.json')):
    label = dump.stem.removesuffix('-events')
    rdc = a.captures / (label + '-rdc/independent_capture.rdc')
    actual = native(label + '-events', dict(action='events', capture=str(rdc.resolve())))
    want = json.loads(dump.read_text(encoding='utf-8'))['reference']
    check(label + '-exact-controller-index', actual['index'] == want)
