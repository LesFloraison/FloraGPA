"""Serial Map/Unmap execution against native and original GPA players."""
import argparse, hashlib, json, os, subprocess, sys
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('probe','replay','oracle-tools','qt-bin','out'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    env=dict(os.environ);env.pop('GPA_LOCAL_INJECT',None);env['PYTHONDONTWRITEBYTECODE']='1';env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH'];checks=[]
    def check(name,ok):
        checks.append(dict(name=name,passed=bool(ok)));(out/'validation.json').write_text(json.dumps(dict(passed=all(x['passed'] for x in checks),scope=__doc__,checks=checks),indent=2))
        if not ok:raise AssertionError(name)
    def run(name,command):
        r=subprocess.run([str(x) for x in command],env=env,capture_output=True,timeout=90);(out/(name+'.log')).write_bytes(r.stdout+b'\n'+r.stderr);check(name,r.returncode==0)
    for mode in ('buffer','texture','nooverwrite','buffer-negative','texture-negative','storage'):
        frame=out/(mode+'.gpa_frame');texture=mode.startswith('texture');negative=mode.endswith('negative')
        run(mode+'-probe',[a.probe.resolve(),frame,mode])
        run(mode+'-flora',[a.replay.resolve(),'replay',frame,'--out',out/(mode+'-flora')])
        run(mode+'-original',[sys.executable,a.oracle_tools.resolve()/'replay_frame.py',frame,'--out',out/(mode+'-original'),'--timeout','60'])
        expected=b''.join(bytes((0,0,255,255) if negative else (255,0,0,255) if (y%2==0 if texture else x<4) else (0,255,0,255)) for y in range(8) for x in range(8))
        native=json.loads((out/(mode+'-flora/report.json')).read_text());original=json.loads((out/(mode+'-original/replay_report.json')).read_text())
        check(mode+'-exact-cpu',(out/(mode+'-flora/frame.rgba')).read_bytes()==expected and original['replayed_rgba_sha256']==hashlib.sha256(expected).hexdigest())
        check(mode+'-real-execution',native['reference_pixels_used'] is False and original['new_framebuffer_count']>0 and original['native']['open_status']==0 and original['native']['playback_status']==0 and original['native']['closed'])
        check(mode+'-reference-control',original['comparison_to_captured_resource']['rgba_equal'] is (not negative))
        counts={'ClearRenderTargetView':1,'Draw':1 if texture else 2,'Present':1}
        if not negative:counts.update(Map=1 if texture else 2,unmap_observations=1 if texture else 2)
        if mode=='storage':counts.update(Map=4,unmap_observations=6,map_read_observations=2,map_read_synchronizations=2)
        check(mode+'-events',native['counts']==counts)
        run(mode+'-inventory',[a.replay.resolve(),'inventory',frame,'--out',out/(mode+'-inventory')])
        inventory=json.loads((out/(mode+'-inventory/report.json')).read_text());check(mode+'-draw-inventory',inventory['draws']==(1 if texture else 2))
        for shader in (r for r in inventory['resources'] if 0x90<=r['type']<=0x95):
            name=mode+'-shader-'+shader['id'];run(name,[a.replay.resolve(),'shader',frame,'--id',shader['id'],'--out',out/name]);check(name+'-dxbc',(out/name/'shader.dxbc').read_bytes().startswith(b'DXBC') and (out/name/'shader.asm').stat().st_size>50)
        check(mode+'-objects-released',json.loads((out/(mode+'-probe.log')).read_text())['source_gpu_objects_alive']==0)
    evidence={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (a.probe,a.replay,a.oracle_tools/'replay_frame.py')};evidence['frames']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.glob('*.gpa_frame')};(out/'inputs.json').write_text(json.dumps(evidence,indent=2));print(json.dumps(dict(passed=True,checks=len(checks))))
if __name__=='__main__':main()
