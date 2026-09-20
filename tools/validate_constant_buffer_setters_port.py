"""Development oracle for six-stage constant-buffer setters and native windows."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--exe',type=Path,required=True)
p.add_argument('--oracle',type=Path,required=True)
p.add_argument('--qt-bin',type=Path)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--real-only',action='store_true')
p.add_argument('--edges-only',action='store_true')
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
from frame import Frame
from engine import Engine
from dx11 import Device
from experiments import Experiment
from setter_edits import captured,validate
from replay_pipeline import inspect
from presentation import select
from validate_class_linkage import clone
from validate_constant_buffer_setters import fixture,values
from constant_buffer_setters import STAGES,TYPES
from validate_buffer_edits import operation

env={k:v for k,v in os.environ.items() if k.upper() in {'SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}}
env['PATH']=os.environ['WINDIR']+'/System32;'+os.environ['WINDIR']
if a.qt_bin:env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH']
checks=[]
def save(completed=False):
    (a.out/'validation.json').write_text(json.dumps(dict(completed=completed,section='real' if a.real_only else 'edges' if a.edges_only else 'all',passed=all(x['passed'] for x in checks),
        executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),checks=checks),indent=2))
def check(name,passed,**details):
    checks.append(dict(name=name,passed=bool(passed),**details));save();print(name,'PASS' if passed else 'FAIL',flush=True)
    assert passed,(name,details)
def project(path,operations,name,cursor=None):
    result=a.out/(name+'.json')
    result.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        cursor=len(operations) if cursor is None else cursor,
        history=[dict(label='Setter migration',operations=[op]) for op in operations])))
    return result
def compare(name,path,driver,project_path,event=None,before=False,pipeline=False,expect_error=False):
    expected=pixels=storage=display=error=None
    with Frame(path) as frame:
        d=Device(driver)
        try:
            engine=Engine(frame,d,experiment=Experiment(frame,project_path) if project_path else None)
            if pipeline:expected=inspect(engine,event,after=not before)
            else:
                engine.replay(event,before=before,readback=False);expected=select(engine,event)
                if expected['resource'] is not None:
                    _,_,pixels=engine.readback(expected['resource'],view=expected['view'])
                    storage,display=engine.output_storage,engine.output_display
        except (ValueError,RuntimeError,KeyError) as caught:
            if not expect_error:raise
            error=str(caught)
        finally:d.close()
    out=a.out/name
    cmd=[str(a.exe.resolve()),'replay-pipeline' if pipeline else 'replay',str(path.resolve()),'--out',str(out.resolve())]
    if driver=='warp':cmd+=['--warp']
    if event is not None:cmd+=['--event',str(event)]
    if before:cmd+=['--before']
    if project_path:cmd+=['--experiment',str(project_path.resolve())]
    run=subprocess.run(cmd,env=env,capture_output=True,timeout=180)
    (a.out/(name+'.log')).write_bytes(run.stdout+run.stderr)
    if expect_error:
        check(name,error is not None and run.returncode!=0,expected_error=error)
        return
    assert run.returncode==0,(name,run.stderr.decode(errors='replace'))
    report=json.loads((out/'report.json').read_text());diff={}
    if pipeline:
        actual=json.loads((out/'replay-pipeline.json').read_text())
        for k in ('event','api','context','value_time','fields','known_fields','unknown_fields','command_enabled','experiment_applied'):
            if actual[k]!=expected[k]:diff[k]=dict(actual=actual[k],expected=expected[k])
    else:
        if report['output_selection']!=expected:diff['selection']=dict(actual=report['output_selection'],expected=expected)
        if report['image_available']!=(pixels is not None):diff['availability']=True
        if pixels is not None:
            if (out/'frame.rgba').read_bytes()!=pixels:diff['pixels']=True
            if (out/'output_storage.bin').read_bytes()!=storage:diff['storage']=True
            if report['output_display']!=display:diff['display']=True
    assert report['reference_pixels_used'] is False
    assert not any(Path(m).name.lower().startswith(('python','gpa_','gpa-','tk8','tcl8','renderdoc')) for m in report['loaded_modules'])
    (out/'expected.json').write_text(json.dumps(expected,indent=2))
    check(name,not diff,differences=diff)

if a.real_only:
    for filename in ('GF2_Exilium_2026_03_03__00_19_35.gpa_frame','bf1_2026_01_21__16_53_05.gpa_frame'):
        path=a.reference/filename
        with Frame(path) as frame:
            ops=[]
            for en in frame.entries.values():
                if en.category==7 and en.type in TYPES:
                    v=captured(en.type,frame.payload(en.id));v['buffers']=[0]*len(v['buffers']);ops.append(dict(kind='setter',event=en.id,values=v))
        proj=project(path,ops,path.stem);undo=project(path,ops,path.stem+'-undo',0)
        compare(path.stem,path,'hardware',proj);compare(path.stem+'-undo',path,'hardware',undo)
        original=(a.out/(path.stem+'-undo')/'frame.rgba').read_bytes();edited=(a.out/path.stem/'frame.rgba').read_bytes()
        golden=('2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1' if filename.startswith('GF2') else '1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6')
        check(path.stem+'-visible-and-undo',edited!=original and hashlib.sha256(original).hexdigest()==golden,setters=len(ops))
    save(True);sys.exit(0)

paths={(s,e,c):fixture(a.out/(s+'-'+e+('-clear' if c else '')+'.gpa_frame'),s,e,c) for s in STAGES for e in ('player','shim','range') for c in (False,True)}
if not a.edges_only:
    cases=[];expected=[]
    def add(path,event,v=...):
        case=dict(path=str(path.resolve()),event=event)
        if v is not ...:case['values']=v
        cases.append(case)
        try:
            with Frame(path) as f:result=captured(f.entries[event].type,f.payload(event)) if v is ... else validate(f,f.entries[event],v)
            expected.append(dict(status='ok',value=result))
        except (ValueError,KeyError,TypeError,OverflowError,struct.error):expected.append(dict(status='error'))
    for stage in STAGES:
        for encoding in ('player','shim','range'):
            path=paths[stage,encoding,False]
            with Frame(path) as f:original=captured(f.entries[90].type,f.payload(90));wire=f.payload(90)
            add(path,90);add(path,90,original)
            for v in (None,[],{},True,{**original,'extra':0}):add(path,90,v)
            for key in original:
                for v in (None,True,'1',-1,2**64,[],{},0,1,[True],[0],[920],[920,922],[0,0],[16,16],[4096,4096],[4112,4112],[2**32-16]*2):add(path,90,{**original,key:v})
            for size in range(len(wire)):
                broken=clone(path,a.out/f'wire-{stage}-{encoding}-{size}.gpa_frame',lambda e,p,size=size:p[:size] if e.id==90 else p);add(broken,90)
            for start in (0,12,13,14):
                for count in (0,1,2,14,15):add(path,90,values([920]*count,start,[16]*count,[16]*count,encoding=='range'))
    request=a.out/'request.json';request.write_text(json.dumps(dict(cases=cases)))
    run=subprocess.run([str(a.oracle.resolve()),'--oracle',str(request.resolve())],env=env,capture_output=True,timeout=180);assert run.returncode==0,run.stderr
    actual=json.loads(run.stdout);diff=[dict(case=c,expected=l,actual=r) for c,l,r in zip(cases,expected,actual,strict=True) if l['status']!=r['status'] or l['status']=='ok' and l['value']!=r.get('value')]
    (a.out/'cpu-differences.json').write_text(json.dumps(diff,indent=2));check('CB wire and argument parity',not diff,cases=len(cases),accepted=sum(x['status']=='ok' for x in expected))

def compare_buffer(name,path,driver,proj,event,resource):
    from buffers import export_buffer
    reference=a.out/(name+'-python');reference.mkdir();out=a.out/name
    with Frame(path) as f:
        d=Device(driver)
        try:expected=export_buffer(Engine(f,d,experiment=Experiment(f,proj)),resource,reference,event)
        finally:d.close()
    cmd=[str(a.exe.resolve()),'buffer',str(path.resolve()),'--id',str(resource),'--event',str(event),'--before','--experiment',str(proj.resolve()),'--out',str(out.resolve())]
    if driver=='warp':cmd+=['--warp']
    run=subprocess.run(cmd,env=env,capture_output=True,timeout=180);(a.out/(name+'.log')).write_bytes(run.stdout+run.stderr);assert run.returncode==0,run.stderr
    actual=json.loads((out/'report.json').read_text());diff={}
    for key in ('constant_bindings','bindings','edit_effect','value_time'):
        if actual[key]!=expected[key]:diff[key]=dict(actual=actual[key],expected=expected[key])
    if (out/'buffer.bin').read_bytes()!=(reference/'buffer.bin').read_bytes():diff['bytes']=True
    (out/'expected.json').write_text(json.dumps(expected,indent=2));check(name,not diff,differences=diff)

if not a.edges_only:
    for (stage,encoding,clear),path in paths.items():
        ops=[dict(kind='setter',event=90,values=values([920,922],2,[16,16],[16,16],encoding=='range')),dict(kind='setter',event=95,values=values([924],13,[16],[16],True))]
        proj=project(path,ops,path.stem+'-edit');undo=project(path,ops,path.stem+'-undo',0)
        for driver in ('hardware','warp'):
            tag=path.stem+'-'+driver
            compare(tag+'-output',path,driver,proj,100)
            for event in (95,100,110,120,130,200,300):compare(tag+'-state-'+str(event),path,driver,proj,event,pipeline=True)
            compare(tag+'-later',path,driver,proj,200);compare(tag+'-undo',path,driver,undo,300)
            if not clear:
                for label,start,ids in [('shrink',3,[922]),('move',4,[922]),('empty',2,[])]:
                    altered=project(path,[dict(kind='setter',event=90,values=values(ids,start,[16]*len(ids),[16]*len(ids),encoding=='range')),ops[1]],tag+'-'+label)
                    compare(tag+'-'+label,path,driver,altered,100,pipeline=True)
path=paths['cs','range',False]
for driver in ('hardware','warp'):
    for label,first,count in [('whole',None,None),('zero',[16,16],[0,0]),('outside',[32,32],[16,16]),('large-window',[16,16],[4096,4096]),('max-offset',[2**32-16]*2,[16]*2)]:
        proj=project(path,[dict(kind='setter',event=90,values=values([920,922],2,first,count,True))],driver+'-'+label)
        compare(driver+'-'+label,path,driver,proj,100);compare(driver+'-'+label+'-state',path,driver,proj,100,pipeline=True);compare_buffer(driver+'-'+label+'-constants',path,driver,proj,100,920)
    setter=dict(kind='setter',event=90,values=values([920,922],2,[16,16],[16,16],True));patch=operation(920,256,struct.pack('<f',.5))
    for label,ops in [('buffer-first',[patch,setter]),('setter-first',[setter,patch])]:
        proj=project(path,ops,driver+'-'+label);compare(driver+'-'+label,path,driver,proj,100);compare_buffer(driver+'-'+label+'-constants',path,driver,proj,100,920)
    proj=project(path,[patch,dict(kind='setter',event=90,values=values([0,922],2,[16,16],[16,16],True))],driver+'-unbound')
    compare(driver+'-unbound',path,driver,proj,100,expect_error=True)
path=fixture(a.out/'unknown.gpa_frame','ps','player',known=False);proj=project(path,[dict(kind='setter',event=90,values=values([922],3))],'unknown-edit')
compare('unknown-displaced-window',path,'warp',proj,90,expect_error=True)
save(True)
