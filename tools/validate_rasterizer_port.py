"""Development-only rasterizer pipeline edit comparisons against preserved Python."""
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
from experiments import Experiment,blob
from validate_buffer_edits import Fixture,experiment
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

from validate_buffer_edits import graphics_fixture, compute_fixture
from validate_rasterizer_extensions import fixture as extension_fixture
from rasterizer_edits import normalize

def compare(name,path,operations,driver,event=100,before=False,target=True,buffer=False,allow_failure=False,expect_failure=False):
    project=a.out/(name+'.json')
    error=None
    with Frame(path) as f:
        experiment(f,project,operations)
        d=create_device(driver)
        try:
            exp=Experiment(f,project)
            e=Engine(f,d,experiment=exp)
            try:
                e.replay(event,before=before,readback=False)
                pixels=e.read_texture(30) if target else None
                storage=e.read_buffer(1000) if buffer else None
                state=inspect_pipeline(Engine(f,d,experiment=exp),event,after=not before)
            except (ValueError,RuntimeError) as ex:
                if not allow_failure:raise
                error=str(ex)
        finally:d.close()
    if expect_failure:assert error,(name,'reference unexpectedly accepted invalid draw')
    label=name+'-'+driver+'-'+str(event)+('-before' if before else '')
    if error:
        native(label+'-rejected',path,'replay',driver,event,before,30 if target else None,project,fail=True)
        check(label+'-rejected',True,reference_error=error)
        return
    if target:
        out,_=native(label+'-image',path,'replay',driver,event,before,30,project)
        check(label+'-image',(out/'frame.rgba').read_bytes()==pixels)
    if buffer:
        out,_=native(label+'-buffer',path,'buffer',driver,event,before,1000,project)
        check(label+'-buffer',(out/'buffer.bin').read_bytes()==storage)
    out,_=native(label+'-pipeline',path,'replay-pipeline',driver,event,before,project=project)
    actual=json.loads((out/'replay-pipeline.json').read_text())
    select=lambda result:{v['field']:v for v in result['fields'] if v['field'] in ('rasterizer.descriptor','depth_state.descriptor','stencil_ref') or v['field'].startswith(('viewports','scissors'))}
    expected=select(state); observed=select(actual)
    assert 'rasterizer.descriptor' in expected and any(k.startswith('viewports') for k in expected),expected
    check(label+'-pipeline',observed==expected,actual=observed,expected=expected)

base=graphics_fixture(a.out/'graphics.gpa_frame')
fields=dict(fill_mode=2,cull_mode=2,front_counter_clockwise=True,depth_bias=-17,
            depth_bias_clamp=.1,slope_scaled_depth_bias=-.2,depth_clip_enable=False,
            scissor_enable=True,multisample_enable=True,antialiased_line_enable=True)
cases=[(key,dict(rasterizer={key:value})) for key,value in fields.items()]
cases += [('all',dict(rasterizer=fields)),('wireframe',dict(wireframe=True)),
          ('solid',dict(wireframe=False)),('cull-none',dict(cull_none=True)),
          ('cull-captured',dict(cull_none=False)),('viewport-none',dict(viewports=[])),
          ('scissor-none',dict(scissors=[],rasterizer=dict(scissor_enable=True))),
          ('viewport',dict(viewports=[[1.25,2.5,4.5,3.25,.25,.75]])),
          ('scissor',dict(scissors=[[2,1,6,5]],rasterizer=dict(scissor_enable=True))),
          ('viewport16',dict(viewports=[[0,0,8,8,0,1]]*16)),
          ('scissor16',dict(scissors=[[-10,-10,3,4]]*16,rasterizer=dict(scissor_enable=True))),
          ('mixed-depth',dict(depth_test=False,stencil_ref=123,rasterizer=dict(cull_mode=3)))]
for name,values in cases:
    for driver in ('hardware','warp'):
        compare(name,base,[dict(kind='pipeline',event=100,values=values)],driver)
ops=[dict(kind='pipeline',event=100,values=dict(rasterizer=dict(cull_mode=3,depth_bias=7),wireframe=True)),
     dict(kind='pipeline',event=100,values=dict(cull_none=False,viewports=[[0,0,5,5,0,1]],depth_test=False)),
     dict(kind='pipeline',event=100,values=dict(wireframe=False))]
for driver in ('hardware','warp'):
    for event,before in ((100,True),(100,False),(200,False)):
        compare('merged',base,ops,driver,event,before)
    for kind in (0x89,0x10e,0x10f):
        path=extension_fixture(a.out/f'{driver}-{kind:x}.gpa_frame',kind=kind)
        compare(f'captured-{kind:x}',path,[],driver,buffer=True)
    for target in (False,True):
        path=extension_fixture(a.out/f'{driver}-target{target}.gpa_frame',target=target)
        for forced in (0,1,2,4,8,16):
            values=dict(rasterizer=dict(forced_sample_count=forced))
            compare(f'target{target}-forced{forced}',path,[dict(kind='pipeline',event=100,values=values)],driver,target=target,buffer=True,allow_failure=True)
        compare(f'target{target}-conservative',path,[dict(kind='pipeline',event=100,values=dict(rasterizer=dict(conservative_raster=1)))],driver,target=target,buffer=True,allow_failure=True)
        compare(f'target{target}-next',path,[dict(kind='pipeline',event=100,values=dict(rasterizer=dict(forced_sample_count=4)))],driver,event=200,target=target,buffer=True)
        compare(f'target{target}-before',path,[dict(kind='pipeline',event=100,values=dict(rasterizer=dict(forced_sample_count=4)))],driver,before=True,target=target,buffer=True)
    for name,opts,extra in [('dsv',dict(depth=True),{}),('depth',{},dict(depth_test=True)),('sample',dict(ps_kind='sample'),{}),('depth-output',dict(ps_kind='depth'),{}),('msaa',dict(samples=4),{})]:
        path=extension_fixture(a.out/f'{driver}-invalid-{name}.gpa_frame',**opts)
        edits=[dict(kind='pipeline',event=100,values=dict(rasterizer=dict(forced_sample_count=4),**extra))]
        compare('guard-'+name,path,edits,driver,allow_failure=True,expect_failure=True)
        # Constraints are submission checks: inspecting or disabling this draw is valid.
        compare('guard-before-'+name,path,edits,driver,before=True,target=name!='msaa',buffer=True)
        compare('guard-disabled-'+name,path,edits+[dict(kind='enabled',event=100,value=False)],driver,target=name!='msaa',buffer=True)
    path=extension_fixture(a.out/f'{driver}-override.gpa_frame')
    for name,source in [('sample','float4 main(uint s:SV_SampleIndex):SV_Target{return s;}'),('depth','float main():SV_Depth{return .5;}')]:
        shader=a.out/f'{driver}-{name}.dxbc';shader.write_bytes(compile_hlsl(source,'ps_5_0')[0])
        compare('override-'+name,path,[dict(kind='shader',resource=12,asset=blob(shader)),dict(kind='pipeline',event=100,values=dict(rasterizer=dict(forced_sample_count=4)))],driver,allow_failure=True,expect_failure=True)
    for kind,values in ((0x10e,dict(forced=4)),(0x10f,dict(conservative=1))):
        path=extension_fixture(a.out/f'{driver}-captured-extended-{kind:x}.gpa_frame',kind=kind,**values)
        compare(f'extended-{kind:x}',path,[dict(kind='pipeline',event=100,values=dict(rasterizer=dict(cull_mode=1)))],driver,buffer=True,allow_failure=True)

invalid=[None,[],{},dict(rasterizer={}),dict(rasterizer=dict(cull_mode=True)),dict(rasterizer=dict(fill_mode=4)),
         dict(rasterizer=dict(depth_bias=2**31)),dict(rasterizer=dict(depth_bias=-2**31-1)),dict(rasterizer=dict(depth_bias_clamp=1e80)),dict(rasterizer=dict(scissor_enable=1)),
         dict(wireframe=True,rasterizer=dict(fill_mode=3)),dict(cull_none=False,rasterizer=dict(cull_mode=2)),
         dict(viewports=[[0,0,-1,1,0,1]]),dict(viewports=[[32767,0,1,1,0,1]]),dict(viewports=[[0,0,1,1,-.1,1]]),
         dict(viewports=[[0,0,1,1,.75,.25]]),dict(viewports=[[0,0,1,1,0,1]]*17),dict(viewports=[[0,0,1,1,0]]),
         dict(scissors=[[0,0,2**31,1]]),dict(scissors=[[2,0,1,1]]),dict(scissors=[[0,0,True,1]]),
         dict(rasterizer=dict(forced_sample_count=3)),dict(rasterizer=dict(conservative_raster=2)),dict(wireframe=1)]
for n,value in enumerate(invalid):
    try:normalize(value);rejected=False
    except ValueError:rejected=True
    project=a.out/f'invalid-{n}.json'
    project.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=__import__('hashlib').sha256(base.read_bytes()).hexdigest(),frame_name=base.name,cursor=1,history=[dict(label='invalid',operations=[dict(kind='pipeline',event=100,values=value)])])))
    native('invalid-'+str(n),base,'replay','warp',100,resource=30,project=project,fail=True);check('invalid-'+str(n),rejected)
compute=compute_fixture(a.out/'compute.gpa_frame')
project=a.out/'dispatch.json'
project.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=__import__('hashlib').sha256(compute.read_bytes()).hexdigest(),frame_name=compute.name,cursor=1,history=[dict(label='dispatch',operations=[dict(kind='pipeline',event=100,values=dict(viewports=[]))])])) )
native('dispatch',compute,'replay','warp',100,project=project,fail=True);check('dispatch-rejected',True)
print(len(checks),'rasterizer comparisons PASS',flush=True)
