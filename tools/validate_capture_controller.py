"""Serial startup-injected capture, independent CPU oracle and both GPA players."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for option in ('bin','replay','oracle-tools','qt-bin','out'):
        p.add_argument('--'+option,type=Path,required=True)
    p.add_argument('--controller-probe',type=Path)
    p.add_argument('--controller',type=Path)
    p.add_argument('--isolated-capture-env',action='store_true')
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);binary=a.bin.resolve()
    env=dict(os.environ);env.pop('GPA_LOCAL_INJECT',None);env['PYTHONDONTWRITEBYTECODE']='1';env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH'];checks=[]
    def check(name,ok):
        checks.append(dict(name=name,passed=bool(ok)));(out/'validation.json').write_text(json.dumps(dict(passed=all(x['passed'] for x in checks),scope=__doc__,checks=checks),indent=2))
        if not ok:raise AssertionError(name)
    def run(name,command,isolated=False):
        child=env
        if isolated:
            child={k:v for k,v in env.items() if k.upper() in {'SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}}
            windows=Path(os.environ['WINDIR']);child['PATH']=str(windows/'System32')+os.pathsep+str(windows)
        r=subprocess.run([str(x) for x in command],env=child,capture_output=True,timeout=120)
        (out/(name+'.log')).write_bytes(r.stdout+b'\n'+r.stderr);check(name,r.returncode==0)
    probe=a.controller_probe.resolve() if a.controller_probe else binary/'FloraCaptureControllerTests.exe'
    controller=a.controller.resolve() if a.controller else binary/'FloraCapture.exe'
    run('injected-controller',[probe,controller,binary/'FloraCaptureLiveTarget.exe',out/'native'],a.isolated_capture_env)
    sessions=json.loads((out/'native/results.json').read_text());total=0
    for session in sessions:
        mode=str(session['mode']);modules=session['module_status']['loaded_modules']
        check(mode+'-independent-target-modules',not any(any(token in m.replace('\\','/').lower() for token in ('intelswtools/gpa/','python3','qt6','renderdoc','floragpa.worker')) for m in modules))
        check(mode+'-no-extra-published-files', len(list((out/'native'/mode/'session').glob('*.gpa_frame')))==len(session['frames']))
        for index,item in enumerate(session['frames']):
            frame=Path(item['path']);name=mode+'-'+str(index);total+=1
            run(name+'-flora',[a.replay.resolve(),'replay',frame,'--out',out/(name+'-flora')])
            run(name+'-original',[sys.executable,a.oracle_tools.resolve()/'replay_frame.py',frame,'--out',out/(name+'-original'),'--timeout','60'])
            flora=json.loads((out/(name+'-flora/report.json')).read_text());original=json.loads((out/(name+'-original/replay_report.json')).read_text());expected=bytes(((255,0,0,255),(0,255,0,255),(0,0,255,255),(255,255,255,255),(0,255,255,255))[(item['present_ordinal']-1)%5] if session['mode']>=2 else (255,0,0,255))*64
            check(name+'-both-players-exact-cpu',(out/(name+'-flora/frame.rgba')).read_bytes()==expected and original['replayed_rgba_sha256']==hashlib.sha256(expected).hexdigest())
            counts={'ClearRenderTargetView':1,'Draw':1,'Present':1}
            if session['mode']>=2 and session['mode']!=8:counts['present_rtv_unbinds']=1
            if 13<=session['mode']<=22:counts['UpdateSubresource1' if session['mode'] in (14,16,18) else 'UpdateSubresource']=1
            elif 11<=session['mode']<=12 or session['mode'] in (26,28):counts.update(Map=1,unmap_observations=1)
            elif session['mode']==27:counts.update(Map=2,unmap_observations=2)
            if 19<=session['mode']<=22:counts['CopySubresourceRegion1' if session['mode'] in (20,22) else 'CopySubresourceRegion']=1
            if 17<=session['mode']<=22:counts.update(query_metadata_records=2,query_end_observations=1,query_completion_unresolved=1)
            check(name+'-actual-events',flora['counts']==counts)
            check(name+'-execution-not-reference',flora['reference_pixels_used'] is False and original['new_framebuffer_count']>0 and original['native']['open_status']==0 and original['native']['playback_status']==0 and original['native']['closed'])
            check(name+'-captured-backbuffer-equal',original['comparison_to_captured_resource']['rgba_equal'] is True)
    evidence={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in (controller,controller.parent/'FloraCaptureRuntime.dll',probe,binary/'FloraCaptureLiveTarget.exe',a.replay,a.oracle_tools/'replay_frame.py')}
    evidence['frame_hashes']={str(p.relative_to(out)):hashlib.sha256(p.read_bytes()).hexdigest() for p in out.rglob('*.gpa_frame')}
    (out/'inputs.json').write_text(json.dumps(evidence,indent=2));print(json.dumps(dict(passed=True,checks=len(checks),frames=total)))


if __name__=='__main__':main()
