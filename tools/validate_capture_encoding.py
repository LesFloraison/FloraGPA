"""Serial dual-player validation of hook-observed scoped recordings; no Present integration."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--replay', type=Path, required=True)
    p.add_argument('--oracle-tools', type=Path, required=True)
    p.add_argument('--qt-bin', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env.pop('GPA_LOCAL_INJECT', None)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + env['PATH']
    checks = []

    def check(name, ok):
        checks.append(dict(name=name, passed=bool(ok)))
        (out/'validation.json').write_text(json.dumps(dict(passed=all(x['passed'] for x in checks),
            scope='Hook-observed scoped Clear/Draw/Dispatch; synthetic presentation; no controller or GF2 acceptance', checks=checks), indent=2))
        if not ok:
            raise AssertionError(name)

    def run(name, command):
        result = subprocess.run([str(x) for x in command], capture_output=True, env=env, timeout=90)
        (out/(name+'.log')).write_bytes(result.stdout+b'\n'+result.stderr)
        check(name, result.returncode == 0)

    for mode in ('clear','draw','negative','dispatch','dispatch-negative'):
        frame = out/(mode+'.gpa_frame')
        run(mode+'-probe', [a.probe.resolve(),frame,mode])
        run(mode+'-flora', [a.replay.resolve(),'replay',frame,'--out',out/(mode+'-flora')])
        run(mode+'-original', [sys.executable,a.oracle_tools.resolve()/'replay_frame.py',frame,
                              '--out',out/(mode+'-original'),'--timeout','60'])
        run(mode+'-inventory', [a.replay.resolve(),'inventory',frame,'--out',out/(mode+'-inventory')])
        inventory=json.loads((out/(mode+'-inventory/report.json')).read_text())
        check(mode+'-draw-dispatch-inventory', inventory['draws']==(1 if mode=='draw' else 0)
              and inventory['dispatches']==(1 if mode=='dispatch' else 0))
        for shader in (r for r in inventory['resources'] if 0x90<=r['type']<=0x95):
            name=mode+'-shader-'+shader['id']
            run(name,[a.replay.resolve(),'shader',frame,'--id',shader['id'],'--out',out/name])
            check(name+'-dxbc-and-disassembly', (out/name/'shader.dxbc').read_bytes().startswith(b'DXBC')
                  and (out/name/'shader.asm').stat().st_size>50)
        expected = bytes((255,0,0,255) if mode=='draw' else (0,255,0,255) if mode=='dispatch' else (0,0,0,255))*64
        independent = (out/(mode+'-flora/frame.rgba')).read_bytes()
        report = json.loads((out/(mode+'-flora/report.json')).read_text())
        original = json.loads((out/(mode+'-original/replay_report.json')).read_text())
        check(mode+'-both-players-match-cpu', independent==expected and
              original['replayed_rgba_sha256']==hashlib.sha256(expected).hexdigest())
        check(mode+'-native-replay-does-not-use-reference', report['reference_pixels_used'] is False)
        check(mode+'-original-kernel-executed', original['new_framebuffer_count']>0 and
              original['native']['open_status']==0 and original['native']['playback_status']==0 and
              original['native']['closed'] is True)
        check(mode+'-flora-independent-modules', not any('intelswtools/gpa/' in x.replace('\\','/').lower()
              or Path(x).name.lower().startswith(('python','qt5')) for x in report['loaded_modules']))
        counts={'ClearRenderTargetView':1,'Present':1}
        if mode in ('draw','dispatch'):counts['Draw' if mode=='draw' else 'Dispatch']=1
        check(mode+'-actual-event-counts', report['counts']==counts)
        if mode.endswith('negative'):
            check(mode+'-reference-cannot-substitute-for-execution',
                  original['comparison_to_captured_resource']['rgba_equal'] is False)
        else:
            check(mode+'-final-output-matches-captured-gpu',
                  original['comparison_to_captured_resource']['rgba_equal'] is True)
        probe=json.loads((out/(mode+'-probe.log')).read_text().strip())
        check(mode+'-source-objects-dead-before-encoding', probe['source_gpu_objects_alive']==0)
    (out/'inputs.json').write_text(json.dumps({str(path):hashlib.sha256(path.read_bytes()).hexdigest()
         for path in (a.probe,a.replay,a.oracle_tools/'replay_frame.py')},indent=2))
    print(json.dumps(dict(passed=True,checks=len(checks))))


if __name__ == '__main__':
    main()
