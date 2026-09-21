"""Compare native post-shader meshes with the original Python worker, serially."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
for key in ('reference', 'exe', 'cli', 'qt-bin', 'captures', 'debug', 'checkpoint', 'out'):
    p.add_argument('--' + key, type=Path, required=True)
p.add_argument('--cases', nargs='+')
p.add_argument('--instances', type=Path, help='Existing two-instance GS capture fixture')
a = p.parse_args()
a.out = a.out.resolve()
a.out.mkdir(parents=True, exist_ok=False)
env = dict(os.environ)
env['PATH'] = str(a.qt_bin.resolve()) + ';C:/Windows/System32;C:/Windows'
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
checks = []
total = dict(comparisons=0, rejected=0, vertices=0, indices=0, bytes=0)


def check(label, ok, **details):
    checks.append(dict(name=label, passed=bool(ok), **details))
    save()
    print(label, 'PASS' if ok else 'FAIL', flush=True)
    assert ok, label


def save(complete=False):
    (a.out / 'validation.json').write_text(json.dumps(dict(
        complete=complete, passed=complete and all(c['passed'] for c in checks),
        **total, checks=checks), indent=2), 'utf-8')


def compare(label, capture, valid=True, **options):
    job = dict(action='postmesh', capture=str(capture.resolve()), gpa_event=None, eid=None,
               instance=0, stage='VSOut')
    job.update(options)
    native = a.out / (label + '-native')
    path = a.out / (label + '-native-job.json')
    path.write_text(json.dumps(dict(job, out=str(native), renderdoc='C:/Program Files/RenderDoc/renderdoc.dll')), 'utf-8')
    run = subprocess.run([str(a.exe.resolve()), '--job', str(path)], env=env, capture_output=True, timeout=180)
    (a.out / (label + '-native.log')).write_bytes(run.stdout + run.stderr)
    actual = json.loads((native / 'result.json').read_text('utf-8'))
    check(label + '-native', actual['ok'] == valid and (run.returncode == 0) == valid, error=actual.get('error'))
    oracle = a.out / (label + '-oracle')
    oracle.mkdir()
    path = a.out / (label + '-oracle-job.json')
    path.write_text(json.dumps(dict(job, out=str(oracle))), 'utf-8')
    script = a.out / (label + '-oracle.py')
    script.write_text('import sys\nsys.path.insert(0,' + repr(str(a.reference / 'standalone')) +
                      ')\nfrom rdc_worker import run\nrun(' + repr(str(path)) + ')\nsys.exit(0)\n', 'utf-8')
    run = subprocess.run(['C:/Program Files/RenderDoc/qrenderdoc.exe', '--python', str(script)],
                         startupinfo=startup, capture_output=True, timeout=180)
    (a.out / (label + '-oracle.log')).write_bytes(run.stdout + run.stderr)
    expected = json.loads((oracle / 'result.json').read_text('utf-8'))
    check(label + '-oracle', run.returncode == 0 and expected['ok'] == valid, error=expected.get('error'))
    if valid:
        fields = ('eid', 'gpa_event', 'gpa_event_map', 'gpa_command_map', 'stage', 'mesh',
                  'vertex_count', 'index_count', 'face_count')
        differences = [k for k in fields if actual.get(k) != expected.get(k)]
        check(label + '-metadata', not differences, differences=differences)
        names = sorted(f.name for f in oracle.glob('post_*'))
        check(label + '-artifacts', names == sorted(f.name for f in native.glob('post_*')))
        for name in names:
            raw, reference = (native / name).read_bytes(), (oracle / name).read_bytes()
            check(label + '-' + name, raw == reference, bytes=len(raw),
                  sha256=hashlib.sha256(raw).hexdigest(), oracle_sha256=hashlib.sha256(reference).hexdigest())
            total['bytes'] += len(raw)
        total['vertices'] += actual['vertex_count']
        total['indices'] += actual['index_count']
        total['comparisons'] += 1
    else:
        total['rejected'] += 1
    modules = [Path(s).name.lower() for s in actual['loaded_modules']]
    check(label + '-runtime', not any(m.startswith(('python', 'gpa_', 'gpa-', 'tk8', 'tcl8')) or
          m in ('gpa.dll', 'dx11_player.dll', 'dx11_playback.dll', 'shimd3d64.dll') for m in modules))


def capture(label, frame, warp=False):
    target = a.out / (label + '-capture')
    command = [str(a.cli.resolve()), 'replay', str(frame.resolve()), '--out', str(target),
               '--renderdoc', 'C:/Program Files/RenderDoc/renderdoc.dll']
    if warp:
        command.append('--warp')
    run = subprocess.run(command, env=env, capture_output=True, timeout=180)
    (a.out / (label + '-capture.log')).write_bytes(run.stdout + run.stderr)
    check(label + '-recapture', run.returncode == 0)
    return target / 'independent_capture.rdc'


cases = [
    ('source-vs', a.debug / 'source-hardware-capture/independent_capture.rdc', True, dict(gpa_event=105)),
    ('source-vs-warp', a.debug / 'source-warp-capture/independent_capture.rdc', True, dict(gpa_event=105)),
    ('index2', a.debug / 'index2-capture/independent_capture.rdc', True, dict(gpa_event=100)),
    ('index4', a.debug / 'index4-capture/independent_capture.rdc', True, dict(gpa_event=100)),
    ('gf2', a.captures / 'gf2-rdc/independent_capture.rdc', True, {}),
    ('bf1', a.captures / 'bf1-rdc/independent_capture.rdc', True, dict(gpa_event=876)),
    ('dispatch', a.debug / 'source-hardware-capture/independent_capture.rdc', False, dict(gpa_event=51)),
    ('missing-event', a.debug / 'source-hardware-capture/independent_capture.rdc', False, dict(gpa_event=999999)),
    ('missing-gs', a.debug / 'source-hardware-capture/independent_capture.rdc', False, dict(gpa_event=105, stage='GSOut')),
    ('bad-stage', a.debug / 'source-hardware-capture/independent_capture.rdc', False, dict(gpa_event=105, stage='invented')),
]
known = {case[0] for case in cases} | {'gs', 'ds', 'gs-warp', 'ds-warp'}
if a.cases and set(a.cases) - known:
    p.error('Unknown cases: ' + ', '.join(sorted(set(a.cases) - known)))
for label, frame, opts in (
    ('gs', a.checkpoint / 'spdb-gs.gpa_frame', dict(stage='GSOut')),
    ('ds', a.checkpoint / 'spdb-ds.gpa_frame', dict(stage='GSOut')),
    ('gs-warp', a.checkpoint / 'spdb-gs.gpa_frame', dict(stage='GSOut')),
    ('ds-warp', a.checkpoint / 'spdb-ds.gpa_frame', dict(stage='GSOut')),
):
    if not a.cases or label in a.cases:
        cases.append((label, capture(label, frame, 'warp' in label), True, opts))
for label, path, valid, options in cases:
    if not a.cases or label in a.cases:
        compare(label, path, valid, **options)
if a.instances:
    for warp in (False, True):
        label = 'instances-warp' if warp else 'instances-hardware'
        rdc = capture(label, a.instances, warp)
        for stage in ('VSOut', 'GSOut'):
            for instance in (0, 1):
                compare(f'{label}-{stage}-{instance}', rdc, gpa_event=100, stage=stage, instance=instance)
save(complete=True)
