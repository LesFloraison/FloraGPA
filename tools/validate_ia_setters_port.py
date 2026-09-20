"""Development oracle for persistent IA setters, output hazards and geometry."""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import random
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
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
from frame import Frame
from engine import Engine
from dx11 import Device
from experiments import Experiment
from setter_edits import captured,validate
from replay_pipeline import inspect
from presentation import select
from validate_buffer_edits import graphics_fixture
from validate_class_linkage import clone
from validate_ia_setters import fixture,vertex
from validate_buffer_edits import operation
from output_setters import captured as output_captured

env={k:v for k,v in os.environ.items() if k.upper() in {'SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}}
env['PATH']=os.environ['WINDIR']+'/System32;'+os.environ['WINDIR']
if a.qt_bin:env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH']
checks=[]
def save(completed=False):
    (a.out/'validation.json').write_text(json.dumps(dict(completed=completed,passed=all(x['passed'] for x in checks),
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

def compare_geometry(name,path,driver,project_path,event):
    from geometry import export_geometry
    reference=a.out/(name+'-python');out=a.out/name
    with Frame(path) as frame:
        d=Device(driver)
        try:
            engine=Engine(frame,d,experiment=Experiment(frame,project_path))
            expected=export_geometry(engine,event,reference)
        finally:d.close()
    cmd=[str(a.exe.resolve()),'geometry',str(path.resolve()),'--event',str(event),'--experiment',str(project_path.resolve()),'--out',str(out.resolve())]
    if driver=='warp':cmd+=['--warp']
    run=subprocess.run(cmd,env=env,capture_output=True,timeout=180)
    (a.out/(name+'.log')).write_bytes(run.stdout+run.stderr)
    assert run.returncode==0,(name,run.stderr.decode(errors='replace'))
    actual=json.loads((out/'geometry.json').read_text())
    fields=('effective_parameters','topology','elements','vertex_references','unique_vertices',
            'strip_restart_references','obj_vertices','obj_faces','obj_lines','obj_points')
    for key in fields:assert actual[key]==expected[key],(name,key,actual[key],expected[key])
    for filename in ('vertices.csv','unique_vertices.csv','references.csv'):
        with (out/filename).open(newline='') as l,(reference/filename).open(newline='') as r:
            for row,(left,right) in enumerate(zip(csv.reader(l),csv.reader(r),strict=True)):
                for x,y in zip(left,right,strict=True):
                    if x!=y:assert row and (float(x)==float(y) or math.isnan(float(x)) and math.isnan(float(y))),(name,filename,x,y)
    if actual['obj_vertices']:
        for left,right in zip((out/'geometry.obj').read_text().splitlines(),(reference/'geometry.obj').read_text().splitlines(),strict=True):
            if left.startswith('#'):continue
            x,y=left.split(),right.split();assert x[0]==y[0] and len(x)==len(y)
            if x[0]=='v':assert all(math.isclose(float(i),float(j),rel_tol=1e-8,abs_tol=1e-8) for i,j in zip(x[1:],y[1:])),name
            else:assert x==y,(name,x,y)
    check(name,True,topology=actual['topology'],vertices=actual['obj_vertices'])

if a.real_only:
    for filename in ('GF2_Exilium_2026_03_03__00_19_35.gpa_frame','bf1_2026_01_21__16_53_05.gpa_frame'):
        path=a.reference/filename
        for kind,label in ((0x34f0,'vertex'),(0x34f1,'index'),(0x34ef,'layout')):
            with Frame(path) as frame:
                ops=[]
                for e in frame.entries.values():
                    if e.category!=7 or e.type!=kind:continue
                    values=captured(kind,frame.payload(e.id))
                    if label=='vertex':values['buffers']=[0]*len(values['buffers'])
                    elif label=='index':values=dict(ib=0,ib_format=0,ib_offset=0)
                    else:values=dict(input_layout=0)
                    ops.append(dict(kind='setter',event=e.id,values=values))
            proj=project(path,ops,path.stem+'-'+label)
            compare(path.stem+'-'+label,path,'hardware',proj)
            undo=project(path,ops,path.stem+'-'+label+'-undo',0)
            compare(path.stem+'-'+label+'-undo',path,'hardware',undo)
            original=(a.out/(path.stem+'-'+label+'-undo')/'frame.rgba').read_bytes()
            edited=(a.out/(path.stem+'-'+label)/'frame.rgba').read_bytes()
            golden='2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1' if filename.startswith('GF2') else '1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6'
            check(path.stem+'-'+label+'-visible-and-undo',edited!=original and hashlib.sha256(original).hexdigest()==golden,setters=len(ops))
    save(True);sys.exit(0)

paths={mode:fixture(a.out/(mode+'.gpa_frame'),mode) for mode in ('persist','restore','clear','unknown','output-first','input-first','om-first','om-last','so-first','so-last','missing','missing-restore','missing-clear','missing-output')}
path=paths['persist'];cases=[];expected=[]
def add(path,event,values=...):
    case=dict(path=str(path.resolve()),event=event)
    if values is not ...:case['values']=values
    cases.append(case)
    try:
        with Frame(path) as frame:
            en=frame.entries[event];value=captured(en.type,frame.payload(event)) if values is ... else validate(frame,en,values)
        expected.append(dict(status='ok',value=value))
    except (ValueError,KeyError,TypeError,OverflowError,struct.error):expected.append(dict(status='error'))
with Frame(path) as frame:originals={event:captured(frame.entries[event].type,frame.payload(event)) for event in (90,91,92)}
for event,original in originals.items():
    add(path,event);add(path,event,original)
    for values in (None,[],{},True,{**original,'extra':0}):add(path,event,values)
    for key in original:
        for value in (None,True,False,'1',-1,2**64,[],{},0,1,20,22,46,60,62,66,2**64-1):add(path,event,{**original,key:value})
    with Frame(path) as frame:wire=frame.payload(event)
    for size in range(len(wire)):
        broken=clone(path,a.out/f'wire-{event}-{size}.gpa_frame',lambda e,p,event=event,size=size:p[:size] if e.id==event else p)
        add(broken,event)
for start in (0,1,30,31,32):
    for count in (0,1,2,31,32,33):
        add(path,90,dict(start_slot=start,buffers=[60]*count,strides=[8]*count,offsets=[0]*count))
for rid in (0,20,22,60,66,70,76,999999):
    for stride in (0,1,2048,2049,2**32-1,2**32):
        add(path,90,dict(start_slot=0,buffers=[rid],strides=[stride],offsets=[2**32-1]))
    for fmt in (0,28,42,57,2**32-1):add(path,91,dict(ib=rid,ib_format=fmt,ib_offset=2**32-1))
for key in ('buffers','strides','offsets'):
    for value in ([True],[-1],[2**64],[0,0],[],[0.0],['0']):add(path,90,dict(start_slot=0,buffers=[60],strides=[8],offsets=[0])|{key:value})
request=a.out/'request.json';request.write_text(json.dumps(dict(cases=cases)))
run=subprocess.run([str(a.oracle.resolve()),'--oracle',str(request.resolve())],env=env,capture_output=True,timeout=180)
assert run.returncode==0,run.stderr
actual=json.loads(run.stdout)
differences=[dict(case=c,expected=l,actual=r) for c,l,r in zip(cases,expected,actual,strict=True) if l['status']!=r['status'] or l['status']=='ok' and l['value']!=r.get('value')]
(a.out/'cpu-differences.json').write_text(json.dumps(differences,indent=2))
check('IA wire and argument parity',not differences,cases=len(cases),accepted=sum(x['status']=='ok' for x in expected))
ops=[dict(kind='setter',event=90,values=dict(start_slot=1,buffers=[60],strides=[8],offsets=[8])),dict(kind='setter',event=91,values=dict(ib=66,ib_format=42,ib_offset=4)),dict(kind='setter',event=92,values=dict(input_layout=62)),dict(kind='setter',event=95,values=dict(start_slot=31,buffers=[60],strides=[8],offsets=[8]))]
for mode in ('persist','restore','clear'):
    path=paths[mode];proj=project(path,ops,mode+'-edit');undo=project(path,ops,mode+'-undo',0)
    for driver in ('warp','hardware'):
        tag=mode+'-'+driver
        for event in (100,200):
            compare(tag+'-output-'+str(event),path,driver,proj,event)
            compare(tag+'-state-'+str(event),path,driver,proj,event,True,True)
        compare(tag+'-setter',path,driver,proj,95,pipeline=True)
        compare(tag+'-undo',path,driver,undo,200)
        if mode=='restore':
            for event in (110,120,140,180,185):compare(tag+'-reset-'+str(event),path,driver,proj,event,pipeline=True)
        if mode=='persist':
            compare_geometry(tag+'-geometry',path,driver,proj,100)
            maximum=project(path,ops[:-1]+[dict(kind='setter',event=95,values=dict(start_slot=31,buffers=[60],strides=[2048],offsets=[2**32-1]))],tag+'-max')
            compare(tag+'-limits',path,driver,maximum,95,pipeline=True)
            for order in (False,True):
                patch=operation(60,8,struct.pack('<6f',5,5,5,6,6,5))
                combined=project(path,([patch]+ops) if order else (ops+[patch]),tag+'-patch-'+str(order))
                compare(tag+'-patch-'+str(order),path,driver,combined,100)
                compare_geometry(tag+'-patch-geometry-'+str(order),path,driver,combined,100)
for mode in ('output-first','input-first','om-first','om-last','so-first','so-last'):
    path=paths[mode];rid=76 if mode.startswith('so-') else 70
    edits=[dict(kind='setter',event=90,values=dict(start_slot=0,buffers=[rid,rid],strides=[8,12],offsets=[12,16])),dict(kind='setter',event=91,values=dict(ib=rid,ib_format=42,ib_offset=3))]
    proj=project(path,edits,mode+'-edit')
    for driver in ('warp','hardware'):
        compare(mode+'-'+driver+'-hazard',path,driver,proj,97,pipeline=True)
        compare(mode+'-'+driver+'-snapshot',path,driver,proj,100,True,True)
        compare(mode+'-'+driver+'-output',path,driver,proj,100)
    # Enable dual output history and remove the original conflict: original and edited inputs diverge.
    event=85 if mode in ('output-first','om-first','so-first') else 96
    with Frame(path) as frame:
        original=output_captured(frame.entries[event].type,frame.payload(event)) if not mode.startswith('so-') else dict(count=1,buffers=[76],offsets=[0])
    if 'uavs' in original:original['uavs']=[0];original['initial_counts']=[0xffffffff]
    else:original=dict(count=0,buffers=[],offsets=[])
    dual=project(path,edits+[dict(kind='setter',event=event,values=original)],mode+'-dual')
    for driver in ('warp','hardware'):
        compare(mode+'-'+driver+'-dual-state',path,driver,dual,100,True,True)
        compare(mode+'-'+driver+'-dual-output',path,driver,dual,100)
for mode in ('missing','missing-restore','missing-clear'):
    path=paths[mode]
    compare(mode+'-unresolved',path,'warp',None,96,expect_error=True)
    if mode!='missing':compare(mode+'-recovered',path,'warp',None,97,pipeline=True)
    compare(mode+'-draw',path,'warp',None,100)
    proj=project(path,[dict(kind='setter',event=96,values=dict(input_layout=62))],mode+'-replace')
    compare(mode+'-replace',path,'warp',proj,100,True,True)
path=paths['unknown'];proj=project(path,ops,'unknown')
compare('unknown-displaced-slot',path,'warp',proj,100,expect_error=True)
path=paths['missing-output']
for safe in (False,True):
    rid=60 if safe else 70
    proj=project(path,[dict(kind='setter',event=90,values=dict(start_slot=0,buffers=[rid,rid],strides=[8,8],offsets=[8,8]))],f'gap-{safe}')
    compare(f'gap-{safe}',path,'warp',proj,100,expect_error=not safe)
save(True)
