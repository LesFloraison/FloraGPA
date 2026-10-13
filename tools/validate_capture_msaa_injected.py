"""Serial startup-injected MSAA captures, exact CPU pixels, both players and Draw-removal controls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


def pixels(samples, ordinal, control='complete'):
    colors=((240,19,73,255),(19,240,73,255),(19,73,240,255),(240,73,19,255),(73,19,240,255))
    result=bytearray()
    for y in range(14):
        for x in range(13*samples):
            px,py,layer,sample=x%13,y%7,y//7,x//13
            p=((px*17+py*3+layer*71+sample*29)&255,(py*31+layer*43+sample*13)&255,(px*7+py*11+layer*19+sample*37)&255,255)
            if control=='missing-initialization':p=(0,0,0,0)
            if px<6 and sample%2==0:p=colors[(ordinal-(2 if control=='missing-application' else 1))%5]
            result.extend(p)
    return result


def records(path):
    raw=path.read_bytes();count=struct.unpack_from('<I',raw,12)[0];offset=struct.unpack_from('<Q',raw,244)[0]
    entries=[struct.unpack_from('<QQIBBH',raw,offset+24*i) for i in range(count)]
    helpers=[];application=[];labels=[];active=False
    for ident,at,size,flags,category,kind in entries:
        if category!=7:continue
        if kind==0x327b:
            label=raw[at+24:at+size].decode('utf-16le').rstrip('\0')
            if label.startswith('FloraCapture initialization: MSAA resource '):active=True;labels.append(label)
        elif kind==0x327c:active=False
        elif kind==0x37:(helpers if active else application).append(ident)
    return raw,offset,entries,helpers,application,labels


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('bin','replay','oracle-tools','qt-bin','out'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--controller',type=Path,help='Use an independently packaged controller/runtime')
    p.add_argument('--sessions',type=Path,help='Validate retained controller results instead of launching new targets')
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);binary=a.bin.resolve()
    env=dict(os.environ);env.pop('GPA_LOCAL_INJECT',None);env['PYTHONDONTWRITEBYTECODE']='1';env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH']
    checks=[];files=[]
    def check(name,ok):
        checks.append(dict(name=name,passed=bool(ok)));(out/'validation.json').write_text(json.dumps(dict(scope=__doc__,passed=all(c['passed'] for c in checks),checks=checks,files=files),indent=2)+'\n')
        if not ok:raise AssertionError(name)
    def run(name,args):
        r=subprocess.run([str(x) for x in args],env=env,capture_output=True,timeout=120);(out/(name+'.log')).write_bytes(r.stdout+b'\n'+r.stderr);check(name,r.returncode==0);return r.stdout
    controller=a.controller.resolve() if a.controller else binary/'FloraCapture.exe'
    sessions_path=a.sessions.resolve() if a.sessions else out/'native/results.json'
    if not a.sessions:run('injected-controller',[binary/'FloraCaptureMsaaControllerTests.exe',controller,binary/'FloraCaptureLiveTarget.exe',out/'native'])
    sessions=json.loads(sessions_path.read_text());check('two-startup-paths',len(sessions)==2 and {s['mode'] for s in sessions}=={24,25})
    for session in sessions:
        mode=session['mode'];samples=2 if mode==24 else 4
        check(f'{mode}-native-after-disconnect',session['target_exit']['ok'] and session['target_exit']['msaa_native_cpu_exact'])
        check(f'{mode}-independent-target',not any(any(t in m.replace('\\','/').lower() for t in ('intelswtools/gpa/','python3','qt6','renderdoc','florareplay','floracore')) for m in session['module_status']['loaded_modules']))
        check(f'{mode}-consecutive-and-recovery',len(session['frames'])>=4 and len(list((sessions_path.parent/str(mode)/'session').glob('*.gpa_frame')))==len(session['frames']))
        cases=[]
        for index,item in enumerate(session['frames']):
            path=Path(item['path']);ordinal=item['present_ordinal'];name=f'{mode}-{index}';raw,offset,entries,helpers,application,labels=records(path)
            check(name+'-recorded-initialization',ordinal>3 and len(helpers)==2*samples and len(application)==3 and len(labels)==1)
            cases.append((name,path,ordinal,'complete',2*samples+3))
            if index==0:
                for control,remove in [('missing-initialization',helpers),('missing-application',application[:2])]:
                    data=bytearray(raw);keep=[entry for entry in entries if entry[0] not in remove];struct.pack_into('<I',data,12,len(keep))
                    for i,entry in enumerate(keep):struct.pack_into('<QQIBBH',data,offset+24*i,*entry)
                    negative=out/(name+'-'+control+'.gpa_frame');negative.write_bytes(data);cases.append((name+'-'+control,negative,ordinal,control,2*samples+3-len(remove)))
        for name,path,ordinal,control,draws in cases:
            expected=pixels(samples,ordinal,control);(out/(name+'.expected.rgba')).write_bytes(expected)
            files.append(dict(file=os.path.relpath(path,out).replace('\\','/'),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),samples=samples,present_ordinal=ordinal,control=control,expected_sha256=hashlib.sha256(expected).hexdigest()))
            run(name+'-flora',[a.replay.resolve(),'replay',path,'--out',out/(name+'-flora')])
            run(name+'-original',[sys.executable,a.oracle_tools.resolve()/'replay_frame.py',path,'--out',out/(name+'-original'),'--timeout','60'])
            flora=json.loads((out/(name+'-flora/report.json')).read_text());original=json.loads((out/(name+'-original/replay_report.json')).read_text())
            check(name+'-exact-cpu', (out/(name+'-flora/frame.rgba')).read_bytes()==expected and original['replayed_rgba_sha256']==hashlib.sha256(expected).hexdigest())
            check(name+'-native-execution',flora['reference_pixels_used'] is False and original['new_framebuffer_count']>0 and original['native']['open_status']==0 and original['native']['playback_status']==0 and original['native']['closed'])
            check(name+'-captured-reference-control',original['comparison_to_captured_resource']['rgba_equal'] is (control=='complete'))
            check(name+'-events',flora['counts']['Draw']==draws and flora['counts']['Present']==1 and flora['counts']['ClearRenderTargetView']==3)
            run(name+'-inventory',[a.replay.resolve(),'inventory',path,'--out',out/(name+'-inventory')]);inventory=json.loads((out/(name+'-inventory/report.json')).read_text())
            check(name+'-inventory',inventory['draws']==draws and inventory['dispatches']==0)
            shaders=[r for r in inventory['resources'] if 0x90<=r['type']<=0x95];check(name+'-application-helper-shaders',len(shaders)>=5)
            for shader in shaders:
                label=name+'-shader-'+shader['id'];run(label,[a.replay.resolve(),'shader',path,'--id',shader['id'],'--out',out/label]);check(label+'-dxbc',(out/label/'shader.dxbc').read_bytes().startswith(b'DXBC') and (out/label/'shader.asm').stat().st_size>50)
            check(name+'-independent-replay',not any('intelswtools/gpa/' in m.replace('\\','/').lower() or Path(m).name.lower().startswith(('python','qt5')) for m in flora['loaded_modules']))
    inputs=[binary/n for n in ('FloraCaptureLiveTarget.exe','FloraCaptureMsaaControllerTests.exe')]+[controller,controller.parent/'FloraCaptureRuntime.dll',sessions_path,a.replay,a.oracle_tools/'replay_frame.py',Path(__file__)]
    (out/'inputs.json').write_text(json.dumps({str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs},indent=2)+'\n')
    print(json.dumps(dict(passed=True,files=len(files),checks=len(checks),captured_files=sum(len(s['frames']) for s in sessions))))


if __name__=='__main__':main()
