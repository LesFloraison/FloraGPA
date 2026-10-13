"""Execute recorded six-stage CB1 windows and removal controls in both players."""
import argparse, hashlib, json, os, struct, subprocess, sys
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('probe','replay','oracle-tools','qt-bin','out'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    env=dict(os.environ);env.pop('GPA_LOCAL_INJECT',None);env['PYTHONDONTWRITEBYTECODE']='1';env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH']
    checks=[];files=[]
    def check(name,ok):
        checks.append(dict(name=name,passed=bool(ok)))
        (out/'validation.json').write_text(json.dumps(dict(passed=all(x['passed'] for x in checks),files=files,checks=checks),indent=2)+'\n')
        if not ok:raise AssertionError(name)
    def run(name,args):
        r=subprocess.run([str(x) for x in args],env=env,capture_output=True,timeout=120)
        (out/(name+'.log')).write_bytes(r.stdout+b'\n'+r.stderr);check(name,r.returncode==0)
    for device in ('warp','hardware'):
        run(device+'-probe',[a.probe.resolve(),*(['--hardware'] if device=='hardware' else []),out/device])
        native=json.loads((out/(device+'-probe.log')).read_text())
        check(device+'-native-oracles',native['passed'] and native['files']==18 and native['encoder_refusals']==30 and native['source_gpu_objects_alive']==0 and all(not x['native_warnings'] for x in native['native_diagnostics']))
        manifest=json.loads((out/device/'manifest.json').read_text());check(device+'-six-stages',len(manifest)==18 and {x['stage'] for x in manifest}==set(range(6)))
        for item in manifest:
            frame=out/device/item['file'];name=device+'-'+frame.stem;expected=frame.with_suffix('.expected.rgba').read_bytes();stage=item['stage']
            files.append(dict(file=frame.relative_to(out).as_posix(),sha256=hashlib.sha256(frame.read_bytes()).hexdigest(),expected_sha256=hashlib.sha256(expected).hexdigest()))
            run(name+'-flora',[a.replay.resolve(),'replay',frame,'--out',out/(name+'-flora')])
            run(name+'-original',[sys.executable,a.oracle_tools.resolve()/'replay_frame.py',frame,'--out',out/(name+'-original'),'--timeout','60'])
            flora=json.loads((out/(name+'-flora/report.json')).read_text());original=json.loads((out/(name+'-original/replay_report.json')).read_text())
            check(name+'-exact-native-cpu',(out/(name+'-flora/frame.rgba')).read_bytes()==expected and original['replayed_rgba_sha256']==hashlib.sha256(expected).hexdigest())
            check(name+'-actual-execution',flora['reference_pixels_used'] is False and original['native']['open_status']==0 and original['native']['playback_status']==0 and original['native']['closed'] and original['new_framebuffer_count']>0)
            check(name+'-negative-reference',original['comparison_to_captured_resource']['rgba_equal'] is (not item['negative']))
            check(name+'-work-counts',flora['counts'].get('Dispatch' if stage==5 else 'Draw',0)==item['work'])
            raw=frame.read_bytes();count=struct.unpack_from('<I',raw,12)[0];offset=struct.unpack_from('<Q',raw,244)[0]
            entries=[struct.unpack_from('<QQIBBH',raw,offset+24*i) for i in range(count)]
            ranges=[e for e in entries if e[4]==7 and e[5]==0x24f+stage];resets=[e for e in entries if e[4]==7 and e[5]==0x249+stage]
            check(name+'-range-and-reset-records',(len(ranges),len(resets))==((7,2) if not item['negative'] else (0,0)))
            run(name+'-inventory',[a.replay.resolve(),'inventory',frame,'--out',out/(name+'-inventory')])
            inventory=json.loads((out/(name+'-inventory/report.json')).read_text());check(name+'-inventory',inventory['dispatches' if stage==5 else 'draws']==item['work'])
            shaders=[r for r in inventory['resources'] if 0x90<=r['type']<=0x95]
            check(name+'-shader-count',len(shaders)==(4 if stage in (1,2) else 3 if stage in (3,5) else 2))
            for shader in shaders:
                label=name+'-shader-'+shader['id'];run(label,[a.replay.resolve(),'shader',frame,'--id',shader['id'],'--out',out/label])
                check(label+'-dxbc',(out/label/'shader.dxbc').read_bytes().startswith(b'DXBC') and (out/label/'shader.asm').stat().st_size>50)
            check(name+'-independent-modules',not any('intelswtools/gpa/' in m.replace('\\','/').lower() or Path(m).name.lower().startswith(('python','qt5')) for m in flora['loaded_modules']))
    paths=(a.probe,a.replay,a.oracle_tools/'replay_frame.py',Path(__file__))
    (out/'inputs.json').write_text(json.dumps({str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in paths},indent=2)+'\n')
    print(json.dumps(dict(passed=True,files=len(files),checks=len(checks))))

if __name__=='__main__':main()
