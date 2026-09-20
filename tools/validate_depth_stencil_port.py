"""Development-only depth/stencil pipeline edit comparisons against preserved Python."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

p=argparse.ArgumentParser()
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--exe',type=Path,required=True)
p.add_argument('--qt-bin',type=Path)
p.add_argument('--isolated-env',action='store_true')
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference.resolve()),str(a.reference.resolve().parent/'tools')]
from frame import Frame
from devices import create_device
from engine import Engine
from predication import inspect_result
from experiments import Experiment,blob
from validate_predication import fixture
from validate_buffer_edits import Fixture,experiment,operation
from validate_so_passthrough import multi_fixture
from replay_pipeline import inspect as inspect_pipeline
from shaders import compile_hlsl

env=dict(os.environ)
if a.isolated_env:
    env={k:v for k,v in env.items() if k.upper() in {'SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}}
    env['PATH']=os.environ['WINDIR']+'/System32;'+os.environ['WINDIR']
if a.qt_bin:env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH']
checks=[]
def check(name,value,**detail):
    checks.append(dict(case=name,passed=bool(value),**detail))
    (a.out/'validation.json').write_text(json.dumps(checks,indent=2)+'\n')
    assert value,checks[-1]
    print(name,'PASS',flush=True)
def native(name,path,action,driver,event=300,before=False,resource=None,project=None,fail=False):
    out=a.out/name
    cmd=[str(a.exe.resolve()),action,str(path.resolve()),'--out',str(out.resolve()),'--event',str(event)]
    if before:cmd+=['--before']
    if resource is not None:cmd+=['--id',str(resource)]
    if driver=='warp':cmd+=['--warp']
    if project:cmd+=['--experiment',str(project.resolve())]
    r=subprocess.run(cmd,env=env,capture_output=True,timeout=90)
    (a.out/(name+'.log')).write_bytes(r.stdout+r.stderr)
    if fail:
        assert r.returncode==1 and b'"completed":false' in r.stderr,(name,r.returncode,r.stderr)
        return
    assert r.returncode==0,(name,r.returncode,r.stderr)
    report=json.loads((out/'report.json').read_text())
    assert report['completed'] and report['reference_pixels_used'] is False
    assert not any(Path(m).name.lower().startswith(('python','gpa_','gpa-','tk8','tcl8')) or Path(m).name.lower()=='renderdoc.dll' for m in report['loaded_modules'])
    return out,report
def clone(path,name,change=lambda e,r:r,extras=lambda x:None):
    x=Fixture();x.records=[]
    with Frame(path) as f:
        for e in f.entries.values():
            raw=change(e,f.payload(e.id))
            if raw is not None:x.add(e.id,e.category,e.type,raw)
    extras(x);out=a.out/(name+'.gpa_frame');x.write(out);return out
from validate_depth_stencil_edits import fixture as depth_fixture
from rasterizer_edits import normalize

def compare_depth(name,path,operations,driver,event=100,before=False):
    project=a.out/(name+'.json')
    with Frame(path) as f:
        experiment(f,project,operations)
        d=create_device(driver)
        try:
            exp=Experiment(f,project)
            e=Engine(f,d,experiment=exp);e.replay(event,before=before,readback=False)
            pixels=e.read_texture(30)
            state=inspect_pipeline(Engine(f,d,experiment=exp),event,after=not before)
        finally:d.close()
    label=name+'-'+driver+'-'+str(event)+('-before' if before else '')
    out,_=native(label+'-image',path,'replay',driver,event,before,30,project)
    check(label+'-image',(out/'frame.rgba').read_bytes()==pixels)
    out,_=native(label+'-pipeline',path,'replay-pipeline',driver,event,before,project=project)
    actual=json.loads((out/'replay-pipeline.json').read_text())
    # Native edited state object identity has no captured resource ID; descriptors are authoritative.
    select=lambda result:{v['field']:v for v in result['fields'] if v['field'] in ('depth_state.descriptor','stencil_ref')}
    check(label+'-depth',select(actual)==select(state),actual=select(actual),expected=select(state))

for fmt in (45,20):
    path=depth_fixture(a.out/f'depth-{fmt}.gpa_frame',fmt)
    cases=[('ref',dict(stencil_ref=0xfedcba98)),('no-write',dict(depth_write=False)),('no-test',dict(depth_test=False)),
           ('masks',dict(stencil_ref=0xa5,depth_stencil=dict(depth_enable=False,stencil_enable=True,stencil_read_mask=15,stencil_write_mask=15,front_face=dict(func=3,fail_op=3))))]
    cases += [('func-'+str(func),dict(depth_stencil=dict(depth_func=func))) for func in range(1,9)]
    for face in ('front_face','back_face'):
        for branch in ('fail_op','depth_fail_op','pass_op'):
            for op in range(1,9):
                cases.append((face+'-'+branch+'-'+str(op),dict(stencil_ref=0xa5,depth_stencil=dict(stencil_enable=True,depth_write_mask=0,depth_func=1 if branch=='depth_fail_op' else 8,**{face:dict(func=1 if branch=='fail_op' else 8,**{branch:op})}))))
    for name,values in cases:
        current=path
        if name.startswith('back_face'):
            def reverse(e,raw):
                if e.id==52:raw=bytearray(raw);struct.pack_into('<I',raw,24,1)
                return raw
            current=clone(path,f'reverse-{fmt}-{name}',reverse)
        for driver in ('hardware','warp'):
            compare_depth(f'{fmt}-{name}',current,[dict(kind='pipeline',event=100,values=values)],driver)
    ops=[dict(kind='pipeline',event=100,values=dict(depth_test=False,depth_stencil=dict(stencil_enable=True,front_face=dict(pass_op=3,func=8)))),dict(kind='pipeline',event=100,values=dict(depth_stencil=dict(front_face=dict(func=3)),stencil_ref=0x53))]
    for driver in ('hardware','warp'):
        for event,before in ((100,True),(100,False),(200,False)):
            compare_depth(f'{fmt}-merged',path,ops,driver,event,before)
invalid=[{},dict(depth_stencil={}),dict(depth_stencil=dict(depth_enable=1)),dict(depth_stencil=dict(depth_write_mask=True)),dict(depth_stencil=dict(depth_func=9)),dict(depth_stencil=dict(stencil_write_mask=256)),dict(depth_stencil=dict(front_face={})),dict(depth_stencil=dict(back_face=dict(pass_op=0))),dict(depth_stencil=dict(front_face=dict(func=1.5))),dict(stencil_ref=-1),dict(stencil_ref=2**32),dict(stencil_ref=True),dict(depth_test=False,depth_stencil=dict(depth_enable=True))]
for n,value in enumerate(invalid):
    try:normalize(value);rejected=False
    except ValueError:rejected=True
    project=a.out/f'invalid-{n}.json'
    project.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=__import__('hashlib').sha256(path.read_bytes()).hexdigest(),frame_name=path.name,cursor=1,history=[dict(label='invalid',operations=[dict(kind='pipeline',event=100,values=value)])])))
    native('invalid-'+str(n),path,'replay','warp',100,resource=30,project=project,fail=True);check('invalid-'+str(n),rejected)
print(len(checks),'depth/stencil comparisons PASS',flush=True)
