"""Serial CopyStructureCount native/dual-player checks with independent byte/pixel oracles."""
import argparse,hashlib,json,os,subprocess,sys
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for k in ('probe','replay','oracle-tools','qt-bin','out'):p.add_argument('--'+k,type=Path,required=True)
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    env=dict(os.environ);env.pop('GPA_LOCAL_INJECT',None);env['PYTHONDONTWRITEBYTECODE']='1';env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH'];checks=[]
    def check(name,ok):
        checks.append(dict(name=name,passed=bool(ok)));(out/'validation.json').write_text(json.dumps(dict(passed=all(x['passed'] for x in checks),checks=checks),indent=2))
        if not ok:raise AssertionError(name)
    def run(name,cmd):
        r=subprocess.run([str(x) for x in cmd],env=env,capture_output=True,timeout=120);(out/(name+'.log')).write_bytes(r.stdout+b'\n'+r.stderr);check(name,r.returncode==0)
    for device in ('warp','hardware'):
        run(device+'-probe',[a.probe.resolve(),out/device]+(['--hardware'] if device=='hardware' else []))
        frames=sorted((out/device).glob('*.gpa_frame'));check(device+'-files',len(frames)==18)
        for frame in frames:
            label=device+'-'+frame.stem;mode=int(frame.stem.split('-')[1]);wrong=frame.stem.endswith('-wrong-offset');no_draw=frame.stem.endswith('-no-draw');repeated=mode==2
            color=(0,0,0,255) if no_draw else (0,0,255,255) if wrong else (0,255,0,255) if repeated else (255,0,0,255);pixels=bytes(color)*64
            run(label+'-flora',[a.replay.resolve(),'replay',frame,'--out',out/(label+'-flora')]);run(label+'-original',[sys.executable,a.oracle_tools.resolve()/'replay_frame.py',frame,'--out',out/(label+'-original'),'--timeout','60'])
            flora=json.loads((out/(label+'-flora/report.json')).read_text());original=json.loads((out/(label+'-original/replay_report.json')).read_text())
            check(label+'-exact-pixels',(out/(label+'-flora/frame.rgba')).read_bytes()==pixels and original['replayed_rgba_sha256']==hashlib.sha256(pixels).hexdigest())
            check(label+'-actual-execution',flora['reference_pixels_used'] is False and original['new_framebuffer_count']>0 and original['native']['open_status']==original['native']['playback_status']==0 and original['native']['closed'])
            check(label+'-reference-control',original['comparison_to_captured_resource']['rgba_equal']==(not wrong and not no_draw))
            resets=5 if mode==0 else 6 if repeated else 3
            counts=dict(CopyStructureCount=10 if repeated else 5,Dispatch=4 if repeated else 2,Present=1,ClearState=resets,annotation_records=2*resets)
            if not no_draw:counts['Draw']=1
            check(label+'-events',flora['counts']==counts)
            run(label+'-inventory',[a.replay.resolve(),'inventory',frame,'--out',out/(label+'-inventory')]);inv=json.loads((out/(label+'-inventory/report.json')).read_text())
            copies=[c for c in inv['commands'] if c['type']==0x3f];check(label+'-normalized-copies',len(copies)==counts['CopyStructureCount'] and all(c['bytes']==36 for c in copies))
            buffers=[r for r in inv['resources'] if r['type']==0x83];check(label+'-two-buffers',len(buffers)==2)
            # This fixture creates source then destination; encoder resource IDs follow creation IDs.
            for kind,r in zip(('source','destination'),buffers):
                name=label+'-'+kind;run(name,[a.replay.resolve(),'buffer',frame,'--id',r['id'],'--event',copies[-1]['id'],'--out',out/name])
                if kind=='source':
                    values=[17 if i in ({2,5,7,8} if repeated else {2,7}) else 0 for i in range(16)]
                    counters=json.loads((out/name/'report.json').read_text())['uav_counters'];check(name+'-hidden-counters',len(counters)==1 and counters[0]['value']==(9 if repeated else 8))
                else:
                    values=[0xdeadbeef]*16;values[1:6]=[5,6,5,8,9] if repeated else [2,3,5,7,8]
                    if wrong:values[6],values[1]=values[1],0xdeadbeef
                check(name+'-exact-bytes',(out/name/'buffer.bin').read_bytes()==b''.join(v.to_bytes(4,'little') for v in values))
            name=label+'-source-a-counter';run(name,[a.replay.resolve(),'buffer',frame,'--id',buffers[0]['id'],'--event',copies[-3]['id'],'--out',out/name])
            a_counters=json.loads((out/name/'report.json').read_text())['uav_counters'];check(name+'-value',len(a_counters)==1 and a_counters[0]['value']==5 and a_counters[0]['view']!=counters[0]['view'])
            shaders=[r for r in inv['resources'] if 0x90<=r['type']<=0x95];check(label+'-three-shaders',len(shaders)==3)
            for shader in shaders:
                name=label+'-shader-'+shader['id'];run(name,[a.replay.resolve(),'shader',frame,'--id',shader['id'],'--out',out/name]);check(name+'-dxbc',(out/name/'shader.dxbc').read_bytes().startswith(b'DXBC') and (out/name/'shader.asm').stat().st_size>50)
    inputs={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (a.probe,a.replay,a.oracle_tools/'replay_frame.py')};inputs['frames']={str(p.relative_to(out)):hashlib.sha256(p.read_bytes()).hexdigest() for p in out.rglob('*.gpa_frame')};(out/'inputs.json').write_text(json.dumps(inputs,indent=2));print(json.dumps(dict(passed=True,files=len(inputs['frames']),checks=len(checks))))
if __name__=='__main__':main()
