"""Development oracle for persistent topology/RS/OM setters, pixels and pipeline inheritance."""
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
from blend_edits import default,pack

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
        with Frame(path) as frame:ops=[dict(kind='setter',event=e.id,values=dict(viewports_values=[])) for e in frame.entries.values() if e.category==7 and e.type==0x350a]
        assert ops
        edited=project(path,ops,path.stem+'-empty');undo=project(path,ops,path.stem+'-undo',0)
        compare(path.stem+'-edited',path,'hardware',edited)
        compare(path.stem+'-undo',path,'hardware',undo)
        changed=(a.out/(path.stem+'-edited')/'frame.rgba').read_bytes()
        original=(a.out/(path.stem+'-undo')/'frame.rgba').read_bytes()
        golden='2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1' if filename.startswith('GF2') else '1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6'
        check(path.stem+'-visible-edit-and-golden-undo',changed!=original and hashlib.sha256(original).hexdigest()==golden,setters=len(ops))
    save(True);sys.exit(0)

base=graphics_fixture(a.out/'base.gpa_frame')
raws={80:(24,struct.pack('<I',4)),81:(43,struct.pack('<Q',52)),82:(44,struct.pack('<IB6f',1,1,0,0,8,8,0,1)),
      83:(45,struct.pack('<IB4i',1,1,0,0,8,8)),84:(35,struct.pack('<QB4fI',0,1,1,1,1,1,0xffffffff)),85:(36,struct.pack('<QI',0,0))}
def additions(f,name):
    blend=default();blend['targets']['0']['write_mask']=1
    f.add(56,5,0x8a,bytes(16)+pack(blend,False))
    f.add(58,5,0x89,bytes(16)+struct.pack('<IIIiffIIII',3,1,0,0,0.,0.,1,1,0,0))
    f.add(60,5,0x8b,bytes(16)+struct.pack('<13I',0,0,8,0,0xffff,1,1,1,8,1,1,1,8))
    for event,(slot,raw) in raws.items():f.add(event,7,0x34de+slot,struct.pack('<QQ',0,1)+raw)
    f.add(150,7,0x32,struct.pack('<QQQB4f',0,1,44,1,0,0,0,0))
    if name=='restore':
        for event,(slot,raw) in raws.items():f.add(event+100,7,0x34de+slot,struct.pack('<QQ',0,1)+raw)
    if name=='clear':f.add(170,7,0x242,struct.pack('<QQ',0,1))
paths={name:clone(base,a.out/(name+'.gpa_frame'),lambda e,p:p,lambda f,name=name:additions(f,name)) for name in ('persist','restore','clear')}
cases=[];expected=[]
def add(path,event,values=...):
    case=dict(path=str(path.resolve()),event=event)
    if values is not ...:case['values']=values
    cases.append(case)
    try:
        with Frame(path) as frame:
            e=frame.entries[event]
            result=captured(e.type,frame.payload(event)) if values is ... else validate(frame,e,values)
        expected.append(dict(status='ok',value=result))
    except (ValueError,KeyError,TypeError,OverflowError,struct.error):expected.append(dict(status='error'))
path=paths['persist']
with Frame(path) as frame:originals={event:captured(frame.entries[event].type,frame.payload(event)) for event in raws}
for event,original in originals.items():
    add(path,event);add(path,event,original)
    for value in (None,[],{},True,{**original,'extra':0}):add(path,event,value)
    for key in original:
        for bad in (None,True,False,'1',-1,2**64,[],{}):add(path,event,{**original,key:bad})
for value in range(72):add(path,80,dict(topology=value))
for event,key in ((81,'rasterizer'),(84,'blend'),(85,'depth_state')):
    for value in (0,1,10,30,44,52,56,58,60,2**64-1):add(path,event,{**originals[event],key:value})
rng=random.Random(20260920)
for _ in range(160):
    v=[rng.uniform(-40000,40000),rng.uniform(-40000,40000),rng.uniform(-1,65535),rng.uniform(-1,65535),rng.uniform(-.1,1.1),rng.uniform(-.1,1.1)]
    add(path,82,dict(viewports_values=[v]))
    v=[rng.randrange(-2**31-1,2**31+1) for _ in range(4)]
    add(path,83,dict(scissors_values=[v]))
for count in (0,1,16,17):
    add(path,82,dict(viewports_values=[[0,0,8,8,0,1]]*count));add(path,83,dict(scissors_values=[[0,0,8,8]]*count))
for x in (0,.1,-1,1e38,1e39):add(path,84,{**originals[84],'blend_factor':[x,x,x,x]})
for event,(slot,raw) in raws.items():
    wire=struct.pack('<QQ',0,1)+raw
    for size in range(len(wire)):
        broken=clone(path,a.out/f'wire-{event}-{size}.gpa_frame',lambda e,p,event=event,size=size:p[:size] if e.id==event else p)
        add(broken,event)
request=a.out/'request.json';request.write_text(json.dumps(dict(cases=cases)))
run=subprocess.run([str(a.oracle.resolve()),'--oracle',str(request.resolve())],env=env,capture_output=True,timeout=180)
assert run.returncode==0,run.stderr
actual=json.loads(run.stdout);differences=[]
for i,(left,right) in enumerate(zip(expected,actual,strict=True)):
    if left['status']!=right['status'] or left['status']=='ok' and left['value']!=right.get('value'):
        differences.append(dict(case=cases[i],expected=left,actual=right))
(a.out/'cpu-differences.json').write_text(json.dumps(differences,indent=2))
check('CPU wire and value parity',not differences,cases=len(cases),accepted=sum(x['status']=='ok' for x in expected))
ops=[dict(kind='setter',event=e,values=v) for e,v in ((81,dict(rasterizer=58)),(82,dict(viewports_values=[[1,2,4,3,0,1]])),
    (83,dict(scissors_values=[[2,1,6,4]])),(84,dict(blend=56,blend_factor=[.25,.5,.75,1],sample_mask=0xffffffff)),(85,dict(depth_state=60,stencil_ref=0x1234)))]
for name,path in paths.items():
    projects={'edited':project(path,ops,name+'-edited'),'undo':project(path,ops,name+'-undo',0),
        'topology':project(path,[dict(kind='setter',event=80,values=dict(topology=1))],name+'-topology'),
        'empty':project(path,[dict(kind='setter',event=82,values=dict(viewports_values=[]))],name+'-empty'),
        'factors':project(path,[dict(kind='setter',event=84,values=dict(blend=0,blend_factor=[-1,2,1e38,.125],sample_mask=0xffffffff))],name+'-factors'),
        'inherit':project(path,[dict(kind='pipeline',event=100,values=dict(rasterizer=dict(depth_bias=2),blend_state=dict(alpha_to_coverage=True),depth_stencil=dict(depth_func=8))),*ops],name+'-inherit')}
    for driver in ('warp','hardware'):
        for label,proj in projects.items():
            for event in (100,200):
                compare(f'{name}-{driver}-{label}-{event}-pixels',path,driver,proj,event)
                compare(f'{name}-{driver}-{label}-{event}-pipeline',path,driver,proj,event,True,True)
        proj=projects['edited']
        for event in (*raws,150,*((180,181,182,183,184,185) if name=='restore' else (170,) if name=='clear' else ())):
            for before in (False,True):compare(f'{name}-{driver}-{event}-{before}-command',path,driver,proj,event,before,True)
invalid=clone(paths['restore'],a.out/'invalid-reset.gpa_frame',lambda e,p:p[:16]+struct.pack('<I',6) if e.id==180 else p)
topology=project(invalid,[dict(kind='setter',event=80,values=dict(topology=1))],'invalid-active')
for driver in ('warp','hardware'):
    compare('invalid-dormant-'+driver,invalid,driver,None,200)
    compare('invalid-active-'+driver,invalid,driver,topology,200,expect_error=True)
    path=paths['persist'];proj=project(path,[dict(kind='setter',event=80,values=dict(topology=1))],driver+'-geometry-project')
    compare_geometry('geometry-'+driver,path,driver,proj,100)
from validate_stream_output import fixture as so_fixture
so_base=a.out/'so-base.gpa_frame';so_fixture(so_base)
so=clone(so_base,a.out/'so-topology.gpa_frame',lambda e,p:p,
         lambda f:f.add(190,7,0x34f6,struct.pack('<QQI',0,1,4)))
proj=project(so,[dict(kind='setter',event=190,values=dict(topology=5))],'so-project')
for driver in ('warp','hardware'):
    compare('so-'+driver+'-pixels',so,driver,proj,200)
    compare('so-'+driver+'-pipeline',so,driver,proj,200,True,True)
    compare_geometry('so-'+driver+'-geometry',so,driver,proj,200)
save(True);print(len(checks),'checks passed',flush=True)
