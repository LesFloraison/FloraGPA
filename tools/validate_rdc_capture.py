"""Optional native RenderDoc recapture checks; Python is only a development oracle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
for name in ('reference', 'exe', 'qt-bin', 'out'):
    p.add_argument('--' + name, type=Path, required=True)
p.add_argument('--renderdoc', type=Path, default=Path('C:/Program Files/RenderDoc/renderdoc.dll'))
p.add_argument('--qrenderdoc', type=Path, default=Path('C:/Program Files/RenderDoc/qrenderdoc.exe'))
p.add_argument('--smoke', action='store_true')
a = p.parse_args()
a.out = a.out.resolve()
a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference / 'standalone'), str(a.reference / 'tools')]
from validate_command_resources import fixture
seed = fixture(a.out / 'commands.gpa_frame')
env = {k: v for k, v in os.environ.items() if k.upper() in
       ('SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'APPDATA', 'LOCALAPPDATA')}
env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + os.environ['WINDIR'] + '/System32;' + os.environ['WINDIR']
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
checks = []
captures = []
def check(name, passed, **details):
    checks.append(dict(name=name, passed=bool(passed), **details))
    (a.out / 'validation.json').write_text(json.dumps(dict(passed=all(x['passed'] for x in checks),
        checks=checks, captures=captures, executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest()), indent=2))
    print(name, 'PASS' if passed else 'FAIL', flush=True)
    if not passed:
        raise AssertionError(name)

msaa = a.reference / 'analysis/capture_samples/msaa/initialized.gpa_frame'
cases = [('msaa', msaa, []), ('commands', seed, [])]
if not a.smoke:
    cases += [('msaa-warp', msaa, ['--warp']), ('commands-warp', seed, ['--warp']),
              ('msaa-测试', msaa, []),
              ('msaa-disabled', msaa, ['--disable', '18']), ('msaa-before', msaa, ['--event', '33', '--before']),
              ('gf2', a.reference / 'GF2_Exilium_2026_03_03__00_19_35.gpa_frame', []),
              ('bf1', a.reference / 'bf1_2026_01_21__16_53_05.gpa_frame', [])]
for name, frame, options in cases:
    results = []
    for capture in (False, True):
        folder = a.out / (name + ('-rdc' if capture else '-baseline'))
        command = [str(a.exe.resolve()), 'replay', str(frame.resolve()), '--out', str(folder)] + options
        if capture:
            command += ['--renderdoc', str(a.renderdoc.resolve())]
        run = subprocess.run(command, env=env, capture_output=True, timeout=240)
        (a.out / (folder.name + '.log')).write_bytes(run.stdout + b'\n' + run.stderr)
        check(folder.name + '-completed', run.returncode == 0)
        result = json.loads((folder / 'report.json').read_text())
        results.append(result)
        modules = [Path(m).name.lower() for m in result['loaded_modules']]
        check(folder.name + '-runtime', not any(m.startswith(('python', 'tk8', 'tcl8')) or
            m in ('gpa.dll', 'dx11_player.dll', 'dx11_playback.dll', 'shimd3d64.dll') for m in modules))
        check(folder.name + '-opt-in', ('renderdoc.dll' in modules) == capture)
    baseline, actual = results
    check(name + '-image-and-commands', all(actual[k] == baseline[k] for k in
        ('rgba_sha256', 'width', 'height', 'counts', 'resource')))
    rdc = Path(actual['rdc_capture'])
    check(name + '-rdc-file', rdc.is_file() and rdc.stat().st_size > 0)
    dump = a.out / (name + '-events.json')
    error = a.out / (name + '-oracle-error.txt')
    script = a.out / (name + '-dump.py')
    args = [str(Path(__file__).with_name('dump_rdc_events.py').resolve()), str(a.reference.resolve()), str(rdc), str(dump)]
    script.write_text('import sys,runpy,traceback\nfrom pathlib import Path\n'
        'sys.argv=' + repr(args) + '\ntry:\n runpy.run_path(sys.argv[0],run_name="__main__")\n'
        'except Exception:\n Path(' + repr(str(error)) + ').write_text(traceback.format_exc(),encoding="utf-8")\n'
        'finally:\n sys.exit(0)\n', encoding='utf-8')
    run = subprocess.run([str(a.qrenderdoc), '--python', str(script)], startupinfo=startup,
                         capture_output=True, timeout=240)
    (a.out / (name + '-oracle.log')).write_bytes(run.stdout + b'\n' + run.stderr)
    check(name + '-reopen', dump.is_file() and not error.exists())
    data = json.loads(dump.read_text(encoding='utf-8'))
    commands = data['reference']['gpa_command_map']
    if name.startswith('msaa'):
        ids = {'1', '18', '33', '39', '43'}
        if name == 'msaa-disabled': ids.remove('18')
        if name == 'msaa-before': ids = {'1', '18'}
        check(name + '-exact-markers', set(commands) == ids and all(c['selectable'] for c in commands.values()))
    check(name + '-resource-identity', any(r['name'].startswith('GPA resource ') for r in data['resources']))
    captures.append(dict(name=name, path=str(rdc), events=str(dump)))

for label, options in [('missing-library', ['--renderdoc', str(a.out / 'missing.dll')]),
                       ('wrong-api', ['--renderdoc', str(Path(os.environ['WINDIR']) / 'System32/version.dll')]),
                       ('discard-on-failure', ['--renderdoc', str(a.renderdoc), '--id', '18446744073709551615'])]:
    folder = a.out / label
    run = subprocess.run([str(a.exe.resolve()), 'replay', str(msaa), '--out', str(folder)] + options,
                         env=env, capture_output=True, timeout=60)
    (a.out / (label + '.log')).write_bytes(run.stdout + b'\n' + run.stderr)
    check(label, run.returncode != 0 and not list(folder.glob('*.rdc')))
check('all-captures-recorded', len(captures) == len(cases))
