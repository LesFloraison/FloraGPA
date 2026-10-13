"""Execute boundary-spanning buffer Map captures and deletion controls in both players."""
import argparse, hashlib, json, os, subprocess, sys
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('probe','replay','oracle-tools','qt-bin','out'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    env=dict(os.environ);env.pop('GPA_LOCAL_INJECT',None);env['PYTHONDONTWRITEBYTECODE']='1';env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH']
    checks=[];files=[]
    def check(name,ok):
        checks.append(dict(name=name,passed=bool(ok)))
        (out/'validation.json').write_text(json.dumps(dict(passed=all(x['passed'] for x in checks),files=files,checks=checks),indent=2))
        if not ok:raise AssertionError(name)
    def run(name,args):
        r=subprocess.run([str(x) for x in args],env=env,capture_output=True,timeout=90)
        (out/(name+'.log')).write_bytes(r.stdout+b'\n'+r.stderr);check(name,r.returncode==0)
    for device in ('warp','hardware'):
        run(device+'-probe',[a.probe.resolve(),'--'+device,out/device])
        probe=json.loads((out/(device+'-probe.log')).read_text())
        check(device+'-native-lifetime-failure-gates',probe['passed'] and probe['encoder_refusals']==42 and probe['source_gpu_objects_alive']==0 and probe['debug_warnings_or_errors']==0)
        manifest=json.loads((out/device/'manifest.json').read_text());check(device+'-eight-files',len(manifest)==8)
        for item in manifest:
            frame=out/device/item['file'];name=device+'-'+frame.stem;negative=item['negative']
            expected=b''.join(bytes((0,0,255,255) if negative and x<4 else (255,0,0,255) if x<4 else (0,255,0,255)) for y in range(8) for x in range(8))
            files.append(dict(file=frame.relative_to(out).as_posix(),sha256=hashlib.sha256(frame.read_bytes()).hexdigest(),expected_sha256=hashlib.sha256(expected).hexdigest()))
            run(name+'-flora',[a.replay.resolve(),'replay',frame,'--out',out/(name+'-flora')])
            run(name+'-original',[sys.executable,a.oracle_tools.resolve()/'replay_frame.py',frame,'--out',out/(name+'-original'),'--timeout','60'])
            flora=json.loads((out/(name+'-flora/report.json')).read_text());original=json.loads((out/(name+'-original/replay_report.json')).read_text())
            check(name+'-exact-cpu-native',(out/(name+'-flora/frame.rgba')).read_bytes()==expected and original['replayed_rgba_sha256']==hashlib.sha256(expected).hexdigest())
            check(name+'-actual-execution',flora['reference_pixels_used'] is False and original['native']['open_status']==0 and original['native']['playback_status']==0 and original['native']['closed'] and original['new_framebuffer_count']>0)
            check(name+'-unchanged-reference-negative',original['comparison_to_captured_resource']['rgba_equal'] is (not negative))
            mappings=(1 if negative else 4)+(3 if item['tail'] else 0)
            counts=flora['counts'];check(name+'-events',counts['DrawIndexed']==2 and counts['ClearRenderTargetView']==2 and counts['Map']==mappings and counts['unmap_observations']==mappings)
            check(name+'-labelled-cleanup',('FloraCapture cleanup: frame-end Map'.encode('utf-16le') in frame.read_bytes()) is item['tail'])
            run(name+'-inventory',[a.replay.resolve(),'inventory',frame,'--out',out/(name+'-inventory')])
            inventory=json.loads((out/(name+'-inventory/report.json')).read_text());check(name+'-draw-inventory',inventory['draws']==2)
            shaders=[r for r in inventory['resources'] if 0x90<=r['type']<=0x95];check(name+'-shaders',len(shaders)==2)
            for shader in shaders:
                label=name+'-shader-'+shader['id'];run(label,[a.replay.resolve(),'shader',frame,'--id',shader['id'],'--out',out/label])
                check(label+'-dxbc',(out/label/'shader.dxbc').read_bytes().startswith(b'DXBC') and (out/label/'shader.asm').stat().st_size>50)
            check(name+'-independent-modules',not any('intelswtools/gpa/' in m.replace('\\','/').lower() or Path(m).name.lower().startswith(('python','qt5')) for m in flora['loaded_modules']))
    paths=(a.probe,a.replay,a.oracle_tools/'replay_frame.py',Path(__file__))
    (out/'inputs.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},indent=2)+'\n')
    print(json.dumps(dict(passed=True,files=len(files),checks=len(checks))))

if __name__=='__main__':main()
