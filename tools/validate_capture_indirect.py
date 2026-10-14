"""Serial GPU-generated indirect capture validation; no substituted direct calls or preview oracle."""
import argparse,hashlib,json,os,subprocess,sys
from pathlib import Path

def argument_words(kind):
    values=[0xdeadbeef]*32;values[4:9]=[0]*5;values[20:25]=[0]*5
    data=([3,1,1],[3,2,1,5],[3,2,1,0xffffffff,5])[kind];values[4:4+len(data)]=data
    return values

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('probe','replay','oracle-tools','qt-bin','out'):p.add_argument('--'+key,type=Path,required=True)
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);env=dict(os.environ);env.pop('GPA_LOCAL_INJECT',None);env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH'];env['PYTHONDONTWRITEBYTECODE']='1';checks=[]
    def check(name,ok):
        checks.append(dict(name=name,passed=bool(ok)));(out/'validation.json').write_text(json.dumps(dict(passed=all(c['passed'] for c in checks),checks=checks),indent=2))
        if not ok:raise AssertionError(name)
    def run(name,cmd):
        r=subprocess.run([str(x) for x in cmd],env=env,capture_output=True,timeout=120);(out/(name+'.log')).write_bytes(r.stdout+b'\n'+r.stderr);check(name,r.returncode==0)
    for device in ('warp','hardware'):
        run(device+'-probe',[a.probe.resolve(),out/device]+(['--hardware'] if device=='hardware' else []));frames=sorted((out/device).glob('*.gpa_frame'));check(device+'-files',len(frames)==27)
        for frame in frames:
            label=device+'-'+frame.stem;kind,mode=map(int,frame.stem.split('-')[:2]);wrong=frame.stem.endswith('-wrong-offset');removed=frame.stem.endswith('-no-indirect');negative=wrong or removed;repeats=2 if mode==2 else 1
            color=((0,0,255,255) if kind==0 else (0,0,0,255)) if negative else ((0,255,0,255) if mode==2 else (255,0,0,255));pixels=bytes(color)*64
            run(label+'-flora',[a.replay.resolve(),'replay',frame,'--out',out/(label+'-flora')]);run(label+'-original',[sys.executable,a.oracle_tools.resolve()/'replay_frame.py',frame,'--out',out/(label+'-original'),'--timeout','60']);flora=json.loads((out/(label+'-flora/report.json')).read_text());original=json.loads((out/(label+'-original/replay_report.json')).read_text())
            check(label+'-exact-pixels',(out/(label+'-flora/frame.rgba')).read_bytes()==pixels and original['replayed_rgba_sha256']==hashlib.sha256(pixels).hexdigest());check(label+'-execution',flora['reference_pixels_used'] is False and original['new_framebuffer_count']>0 and original['native']['open_status']==original['native']['playback_status']==0 and original['native']['closed']);check(label+'-preview-control',original['comparison_to_captured_resource']['rgba_equal']==(not negative))
            name=('DispatchIndirect','DrawInstancedIndirect','DrawIndexedInstancedIndirect')[kind];counts=dict(CopyStructureCount=repeats,Dispatch=repeats,ClearState=repeats,annotation_records=2*repeats,Present=1)
            if not removed:counts[name]=repeats
            if kind==0:counts['Draw']=1
            check(label+'-events',flora['counts']==counts)
            run(label+'-inventory',[a.replay.resolve(),'inventory',frame,'--out',out/(label+'-inventory')]);inv=json.loads((out/(label+'-inventory/report.json')).read_text());events=[c for c in inv['commands'] if c['type'] in (0x36,0x3b,0x3d)];check(label+'-indirect-wire',len(events)==(0 if removed else repeats) and all(c['type']==(0x36,0x3d,0x3b)[kind] and c['bytes']==36 and c['parameters']==[80 if wrong else 16] for c in events))
            buffers=[r for r in inv['resources'] if r['type']==0x83];last=[c for c in inv['commands'] if c['type'] in (0x35,0x36,0x3b,0x3d)][-1]['id'];name=label+'-arguments';run(name,[a.replay.resolve(),'buffer',frame,'--id',buffers[0]['id'],'--event',last,'--out',out/name]);check(name+'-exact-bytes',(out/name/'buffer.bin').read_bytes()==b''.join(v.to_bytes(4,'little') for v in argument_words(kind)))
            # The first count copy precedes the producer shader. Its destination
            # must still contain zero dispatch/instance dimensions, not final data.
            first_copy=next(c['id'] for c in inv['commands'] if c['type']==0x3f);initial=[0xdeadbeef]*32;initial[4:9]=[3,0,0,0,0];initial[20:25]=[0]*5;name=label+'-before-producer';run(name,[a.replay.resolve(),'buffer',frame,'--id',buffers[0]['id'],'--event',first_copy,'--out',out/name]);check(name+'-exact-initial',(out/name/'buffer.bin').read_bytes()==b''.join(v.to_bytes(4,'little') for v in initial))
            if kind==0:
                name=label+'-result';run(name,[a.replay.resolve(),'buffer',frame,'--id',buffers[2]['id'],'--event',last,'--out',out/name]);values=[17 if not negative and i<3*repeats else 0 for i in range(16)];check(name+'-exact-bytes',(out/name/'buffer.bin').read_bytes()==b''.join(v.to_bytes(4,'little') for v in values))
                if not removed:
                    counters=json.loads((out/name/'report.json').read_text())['uav_counters'];check(name+'-counter',len(counters)==1 and counters[0]['value']==(3*(repeats-1) if wrong else 3*repeats))
            shaders=[r for r in inv['resources'] if 0x90<=r['type']<=0x95];check(label+'-shader-count',len(shaders)==(4 if kind==0 else 3))
            for shader in shaders:
                name=label+'-shader-'+shader['id'];run(name,[a.replay.resolve(),'shader',frame,'--id',shader['id'],'--out',out/name]);check(name+'-dxbc',(out/name/'shader.dxbc').read_bytes().startswith(b'DXBC') and (out/name/'shader.asm').stat().st_size>50)
    inputs={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (a.probe,a.replay,a.oracle_tools/'replay_frame.py')};inputs['frames']={str(p.relative_to(out)):hashlib.sha256(p.read_bytes()).hexdigest() for p in out.rglob('*.gpa_frame')};(out/'inputs.json').write_text(json.dumps(inputs,indent=2));print(json.dumps(dict(passed=True,files=len(inputs['frames']),checks=len(checks))))
if __name__=='__main__':main()
