"""Serial dual-player gate for native Present-boundary transactions (internal control API)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for option in ('probe','replay','oracle-tools','qt-bin','out'):
        p.add_argument('--'+option,type=Path,required=True)
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    env=dict(os.environ);env.pop('GPA_LOCAL_INJECT',None);env['PYTHONDONTWRITEBYTECODE']='1'
    env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH'];checks=[]
    def check(name,ok):
        checks.append(dict(name=name,passed=bool(ok)))
        (out/'validation.json').write_text(json.dumps(dict(passed=all(x['passed'] for x in checks),scope=__doc__,checks=checks),indent=2))
        if not ok:raise AssertionError(name)
    def run(name,command):
        r=subprocess.run([str(x) for x in command],env=env,capture_output=True,timeout=90)
        (out/(name+'.log')).write_bytes(r.stdout+b'\n'+r.stderr);check(name,r.returncode==0)
    run('native-transactions',[a.probe.resolve(),out/'native'])
    for index in range(3):
        frame=out/'native'/('frame-'+str(index)+'.gpa_frame');name=str(index)
        run(name+'-flora',[a.replay.resolve(),'replay',frame,'--out',out/(name+'-flora')])
        run(name+'-original',[sys.executable,a.oracle_tools.resolve()/'replay_frame.py',frame,'--out',out/(name+'-original'),'--timeout','60'])
        flora=json.loads((out/(name+'-flora/report.json')).read_text());original=json.loads((out/(name+'-original/replay_report.json')).read_text())
        expected=bytes((255,0,0,255))*64
        check(name+'-both-players-exact-cpu', (out/(name+'-flora/frame.rgba')).read_bytes()==expected and original['replayed_rgba_sha256']==hashlib.sha256(expected).hexdigest())
        check(name+'-recorded-events', flora['counts']=={'ClearRenderTargetView':1,'Draw':1,'Present':1})
        check(name+'-native-execution', flora['reference_pixels_used'] is False)
        check(name+'-original-execution', original['native']['open_status']==0 and original['native']['playback_status']==0 and original['native']['closed'] and original['new_framebuffer_count']>0)
        check(name+'-actual-captured-backbuffer', original['comparison_to_captured_resource']['rgba_equal'] is True)
    evidence={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in (a.probe,a.replay,a.oracle_tools/'replay_frame.py')}
    evidence['frames']={path.name:hashlib.sha256(path.read_bytes()).hexdigest() for path in (out/'native').glob('*.gpa_frame')}
    (out/'inputs.json').write_text(json.dumps(evidence,indent=2));print(json.dumps(dict(passed=True,checks=len(checks))))


if __name__=='__main__':main()
