"""Development-only annotation hierarchy, identity and export parity."""
import argparse,csv,hashlib,json,os,struct,subprocess,sys,uuid
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--exe',type=Path,required=True)
p.add_argument('--qt-bin',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
from annotations import build,export
from frame import Frame
from validate_buffer_edits import Fixture
from validate_annotations import marker
from engine import Engine
from dx11 import Device
env={k:v for k,v in os.environ.items() if k.upper() in ('SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA')}
env['PATH']=str(a.qt_bin.resolve())+os.pathsep+os.environ['WINDIR']+'/System32;'+os.environ['WINDIR']
checks=[]
def save(completed=False):
    (a.out/'validation.json').write_text(json.dumps(dict(completed=completed,passed=all(x['passed'] for x in checks),checks=checks,
        executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest()),indent=2))
def compare(path,label):
    expected=a.out/(label+'-python');actual=a.out/(label+'-native')
    with Frame(path) as frame:want=export(build(frame),expected)
    run=subprocess.run([str(a.exe.resolve()),'annotations',str(path.resolve()),'--out',str(actual.resolve())],env=env,capture_output=True,timeout=120)
    (a.out/(label+'.log')).write_bytes(run.stdout+run.stderr);diff=[]
    if run.returncode:diff.append(dict(error=run.stderr.decode(errors='replace')))
    else:
        got=json.loads((actual/'annotations.json').read_text(encoding='utf-8'))
        # Checked readers have different diagnostic wording; compare its presence
        # while preserving exact malformed status, boundaries and all other data.
        for report in (want,got):
            for n in report['nodes']:
                if n.get('error'):n['error']='<nonempty diagnostic>'
        def differences(w,g,path=''):
            if isinstance(w,dict) and isinstance(g,dict):
                if set(w)!=set(g):diff.append(dict(path=path,expected_keys=sorted(w),actual_keys=sorted(g)))
                for k in sorted(set(w)|set(g)):differences(w.get(k),g.get(k),path+'/'+k)
            elif isinstance(w,list) and isinstance(g,list) and len(w)==len(g):
                for i,(x,y) in enumerate(zip(w,g)):differences(x,y,path+'/'+str(i))
            elif w!=g:diff.append(dict(path=path,expected=w,actual=g))
        differences(want,got)
        def table(folder):
            with (folder/'annotations.csv').open(encoding='utf-8-sig',newline='') as f:rows=list(csv.DictReader(f))
            for row in rows:
                for k in ('context_ids','context_proof_events','draw_ids','unmatched_draw_ids'):row[k]=json.loads(row[k])
            return rows
        differences(table(expected),table(actual),'csv')
        report=json.loads((actual/'report.json').read_text())
        modules=[Path(x).name.lower() for x in report['loaded_modules']]
        if any(x.startswith(('python','gpa-','gpa_','tk8','tcl8')) or x=='renderdoc.dll' for x in modules):diff.append(dict(runtime=modules))
    checks.append(dict(name=label,passed=not diff,differences=diff));save();print(label,'PASS' if not diff else 'FAIL',flush=True)
for folder in ('annotations-v3','annotation-contexts-v5'):
    paths=sorted((a.reference/'output'/folder).glob('*.gpa_frame'))
    if not paths:raise RuntimeError('Missing reference fixtures: '+folder)
    for path in paths:compare(path,folder+'-'+path.stem)
for name in ('GF2_Exilium_2026_03_03__00_19_35.gpa_frame','bf1_2026_01_21__16_53_05.gpa_frame'):compare(a.reference/name,name.split('_')[0])
# Owner/context/event IDs must survive the entire JSON and CSV path as uint64.
f=Fixture();f.records=[];base=1<<63;owner=base+600;ctx=base+1
f.add(ctx,5,0x99,bytes(24));f.add(base+9,3,3,bytes(22320))
f.add(base+10,7,0x3278,struct.pack('<QQi16sQ',0,owner,0,uuid.UUID('c0bfa96c-e089-44fb-8eaf-26f8796190da').bytes_le,ctx))
f.add(base+20,7,0x327b,marker(0x327b,owner,'High IDs'))
f.add(base+30,7,0x37,struct.pack('<QQQII',base+9,0,ctx,3,0))
f.add(base+40,7,0x327c,marker(0x327c,owner))
compare(f.write(a.out/'uint64.gpa_frame'),'uint64')
def replay_compare(path,label,driver,event=None,before=False):
    error=None;image=None;count=None
    with Frame(path) as frame:
        device=Device(driver)
        try:
            engine=Engine(frame,device)
            try:
                image=engine.replay(until=event,before=before)[2]
                count=engine.counts.get('annotation_records',0)
            except (ValueError,RuntimeError,KeyError) as e:error=str(e)
        finally:device.close()
    out=a.out/(label+'-native');cmd=[str(a.exe.resolve()),'replay',str(path.resolve()),'--out',str(out.resolve())]
    if driver=='warp':cmd+=['--warp']
    if event is not None:cmd+=['--event',str(event)]
    if before:cmd+=['--before']
    run=subprocess.run(cmd,env=env,capture_output=True,timeout=120)
    (a.out/(label+'.log')).write_bytes(run.stdout+run.stderr);diff=[]
    if error:
        diagnostics=[]
        for line in run.stderr.decode(errors='replace').splitlines():
            try:diagnostics.append(json.loads(line))
            except ValueError:pass
        if run.returncode!=1 or not any(isinstance(d,dict) and d.get('completed') is False and d.get('error') for d in diagnostics):
            diff.append(dict(rejection=run.returncode))
    elif run.returncode:diff.append(dict(error=run.stderr.decode(errors='replace')))
    else:
        report=json.loads((out/'report.json').read_text());actual=(out/'frame.rgba').read_bytes()
        if actual!=image:diff.append(dict(pixels='mismatch'))
        if report['counts'].get('annotation_records',0)!=count:diff.append(dict(count=dict(expected=count,actual=report['counts'])))
        modules=[Path(x).name.lower() for x in report['loaded_modules']]
        if any(x.startswith(('python','gpa-','gpa_','tk8','tcl8')) or x=='renderdoc.dll' for x in modules):diff.append(dict(runtime=modules))
    checks.append(dict(name=label,passed=not diff,expected_error=error,differences=diff));save();print(label,'PASS' if not diff else 'FAIL',flush=True)
root=a.reference/'output/annotations-v3'
for driver in ('hardware','warp'):
    replay_compare(root/'annotations.gpa_frame',driver+'-replay',driver)
    replay_compare(root/'annotations-base.gpa_frame',driver+'-baseline',driver)
    replay_compare(root/'annotations.gpa_frame',driver+'-before-end',driver,event=220,before=True)
    for kind in range(0x3278,0x327f):replay_compare(root/f'bad-{kind:x}.gpa_frame',driver+f'-reject-{kind:x}',driver)
save(True);print('Checks',len(checks),'failures',sum(not c['passed'] for c in checks),flush=True)
raise SystemExit(0 if all(c['passed'] for c in checks) else 1)
