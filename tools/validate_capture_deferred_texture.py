"""Serial dual-player execution of the native deferred texture fixture's existing files."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('capture-root','probe','replay','oracle-tools','qt-bin','out'):
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    env=dict(os.environ);env.pop('GPA_LOCAL_INJECT',None);env['PYTHONDONTWRITEBYTECODE']='1';env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH']
    checks=[];frames={}
    def check(name,ok):
        checks.append({'name':name,'passed':bool(ok)})
        (out/'validation.json').write_text(json.dumps({'passed':all(c['passed'] for c in checks),'checks':checks},indent=2)+'\n')
        if not ok:raise AssertionError(name)
    def run(name,args):
        result=subprocess.run([str(x) for x in args],env=env,capture_output=True,timeout=120)
        (out/(name+'.log')).write_bytes(result.stdout+b'\n'+result.stderr);check(name,result.returncode==0)
    colors=[bytes(c) for c in ((255,0,0,255),(0,255,0,255),(0,0,255,255),(255,255,255,255))]
    for device in ('warp','hardware'):
        root=a.capture_root/device;rows=json.loads((root/'results.json').read_text())
        check(device+'-six-profiles',len(rows)==6 and {r['mode'] for r in rows}==set(range(1,7)))
        for row in rows:
            mode=row['mode'];directory=root/str(mode);prefix=device+'-'+str(mode)
            if row['refused']:
                check(prefix+'-no-published-file',not directory.exists());continue
            width=8 if mode==4 else 7;height=1 if mode==1 else 8 if mode==4 else 3;depth=2 if mode in (3,6) else 1
            for kind in ('aba','bab','missing-map','missing-draws'):
                name=prefix+'-'+kind;frame=directory/(kind+'.gpa_frame');frames[str(frame)]=hashlib.sha256(frame.read_bytes()).hexdigest()
                version=1 if kind in ('bab','missing-map') else 0
                expected=(bytes((0,0,0,255))*width*height*depth if kind=='missing-draws' else
                          b''.join(colors[(x//4+y//4+version)%4 if mode==4 else (x+y+z+version)%4]
                                   for z in range(depth) for y in range(height) for x in range(width)))
                run(name+'-flora',[a.replay.resolve(),'replay',frame.resolve(),'--out',out/(name+'-flora')])
                run(name+'-original',[sys.executable,a.oracle_tools.resolve()/'replay_frame.py',frame.resolve(),'--out',out/(name+'-original'),'--timeout','60'])
                flora=json.loads((out/(name+'-flora/report.json')).read_text());original=json.loads((out/(name+'-original/replay_report.json')).read_text())
                check(name+'-exact-cpu-pixels',(out/(name+'-flora/frame.rgba')).read_bytes()==expected and original['replayed_rgba_sha256']==hashlib.sha256(expected).hexdigest())
                check(name+'-actual-execution',flora['reference_pixels_used'] is False and original['native']['open_status']==0 and original['native']['playback_status']==0 and original['native']['closed'] and original['new_framebuffer_count']>0)
                check(name+'-preview-control',original['comparison_to_captured_resource']['rgba_equal'] is (not kind.startswith('missing-')))
                maps=2 if kind=='missing-map' else 3;draws=0 if kind=='missing-draws' else 3
                counts={'Map':maps,'unmap_observations':maps,'Present':1}
                if draws:counts['Draw']=draws
                check(name+'-event-counts',flora['counts']==counts)
                run(name+'-inventory',[a.replay.resolve(),'inventory',frame.resolve(),'--out',out/(name+'-inventory')])
                inventory=json.loads((out/(name+'-inventory/report.json')).read_text());shaders=[r for r in inventory['resources'] if 0x90<=r['type']<=0x95]
                check(name+'-inventory',inventory['draws']==draws and inventory['dispatches']==0 and len(shaders)==2)
                for shader in shaders:
                    label=name+'-shader-'+shader['id'];run(label,[a.replay.resolve(),'shader',frame.resolve(),'--id',shader['id'],'--out',out/label])
                    check(label+'-dxbc-assembly',(out/label/'shader.dxbc').read_bytes().startswith(b'DXBC') and (out/label/'shader.asm').stat().st_size>50)
                check(name+'-independent-modules',not any('intelswtools/gpa/' in m.replace('\\','/').lower() or Path(m).name.lower().startswith(('python','qt5','floracapture')) for m in flora['loaded_modules']))
    inputs={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in (a.probe,a.replay,a.oracle_tools/'replay_frame.py')}
    inputs['frames']=frames;(out/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
    print(json.dumps({'passed':True,'files':len(frames),'checks':len(checks)}))

if __name__=='__main__':main()
