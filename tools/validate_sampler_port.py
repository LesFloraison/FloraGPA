"""Development-only sampler descriptor and setter comparisons against preserved Python."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

p=argparse.ArgumentParser()
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--captures',type=Path)
p.add_argument('--real-only',action='store_true')
p.add_argument('--exe',type=Path,required=True)
p.add_argument('--qt-bin',type=Path)
p.add_argument('--isolated-env',action='store_true')
p.add_argument('--out',type=Path,required=True)
a=p.parse_args()
if a.real_only and not a.captures:p.error('--real-only requires --captures')
a.out.mkdir(parents=True,exist_ok=False)
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
    cmd=[str(a.exe.resolve()),action,str(path.resolve()),'--out',str(out.resolve())]
    if event:cmd+=['--event',str(event)]
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

from validate_sampler_setters import fixture as setter_fixture
from validate_sampler_edits import sample_fixture
from sampler_edits import FILTERS

def compare(name,path,operations,driver,event=100,before=False,resource=30,buffer=False,expect_failure=False,pixels=True):
    project=a.out/(name+'.json');error=None
    with Frame(path) as f:
        try:
            experiment(f,project,operations)
            d=create_device(driver)
            try:
                exp=Experiment(f,project);e=Engine(f,d,experiment=exp)
                e.replay(event,before=before,readback=False)
                expected=(e.read_buffer(resource) if buffer else e.read_texture(resource)) if pixels else None
                state=inspect_pipeline(Engine(f,d,experiment=exp),event,after=not before)
            finally:d.close()
        except (ValueError,RuntimeError) as ex:
            if not expect_failure:raise
            error=str(ex)
            if not project.exists():
                project.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=__import__('hashlib').sha256(path.read_bytes()).hexdigest(),frame_name=path.name,cursor=1,history=[dict(label='invalid',operations=operations)])))
    label=name+'-'+driver+'-'+str(event)+('-before' if before else '')
    if error:
        native(label,path,'replay-pipeline',driver,event,before,project=project,fail=True)
        check(label,True,reference_error=error);return
    out,report=native(label+'-pipeline',path,'replay-pipeline',driver,event,before,project=project)
    actual=json.loads((out/'replay-pipeline.json').read_text())
    # Pointer tokens have process-local identities. Compare descriptor/provenance and
    # the equivalence classes of all 96 sampler slots instead of pointer numbers.
    def select(result):
        rows=[v for v in result['fields'] if '.samplers.' in v['field']]
        assert len(rows)==96
        groups={};out={}
        for row in rows:
            v=json.loads(json.dumps(row));obj=v.get('object')
            if obj:
                token=v['value'];groups.setdefault(str(token),len(groups)+1)
                v['value']=groups[str(token)]
                for key in ('token','runtime_token'):obj.pop(key,None)
            out[v['field']]=v
        return out
    observed=select(actual);wanted=select(state)
    check(label+'-bindings',observed==wanted,expected=wanted,actual=observed)
    if pixels:
        out,_=native(label+'-output',path,'buffer' if buffer else 'texture',driver,event,before,resource,project)
        raw=(out/('buffer.bin' if buffer else 'frame.rgba')).read_bytes()
        check(label+'-output',raw==expected,expected_sha256=__import__('hashlib').sha256(expected).hexdigest(),actual_sha256=__import__('hashlib').sha256(raw).hexdigest())

def setter(event,start,ids):return dict(kind='setter',event=event,values=dict(start_slot=start,samplers=ids))
def sampler(stage,slot,**values):return dict(kind='sampler',event=100,stage=stage,slot=slot,values=values)

if not a.real_only:
    paths={}
    for stage in ('vs','hs','ds','gs','ps','cs'):
        for clear in (False,True):
            name=stage+('-clear' if clear else '')
            path=setter_fixture(a.out/(name+'.gpa_frame'),stage,clear=clear);paths[stage,clear]=path
            ops=[setter(90,2,[910,911]),setter(95,15,[912])]
            descriptors=[sampler(stage,slot,filter=0,address_u=4,address_v=4,address_w=4,border_color=color)
                for slot,color in [(2,[.125,0,0,0]),(3,[0,.1875,0,0]),(15,[0,0,.09375,0])]]
            for driver in ('hardware','warp'):
                kwargs=dict(resource=74 if stage=='cs' else 30,buffer=stage=='cs')
                for event,before in [(90,True),(90,False),(95,False),(100,True),(100,False),(110,False),(120,False),(130,False),(140,False),(200,False),(300,False)]:
                    compare(name+'-setters',path,ops,driver,event,before,**kwargs)
                compare(name+'-descriptors',path,descriptors,driver,**kwargs)
                compare(name+'-descriptors-next',path,descriptors,driver,200,**kwargs)
                if not clear:
                    for label,start,ids in [('shrink',3,[911]),('move',4,[911]),('empty',2,[])]:
                        compare(name+'-'+label,path,[setter(90,start,ids),ops[1]],driver,**kwargs)
                    compare(name+'-descriptor-disabled',path,descriptors+[dict(kind='enabled',event=100,value=False)],driver,**kwargs)

    unknown=setter_fixture(a.out/'unknown.gpa_frame','ps',known=False)
    compare('unknown',unknown,[setter(90,3,[911])],'warp',90,expect_failure=True)
    future=setter_fixture(a.out/'future.gpa_frame','ps',future_invalid=True)
    compare('future-early',future,[setter(90,3,[911])],'warp')
    compare('future-invalid',future,[setter(90,3,[911])],'warp',110,expect_failure=True)

    for reverse in (False,True):
        ops=[setter(90,2,[910,911]),sampler('ps',2,border_color=[.25,0,0,0])]
        if reverse:ops.reverse()
        for driver in ('hardware','warp'):compare('mixed-'+str(reverse),paths['ps',False],ops,driver)
    compare('inherited-lod-invalid',paths['ps',False],[sampler('ps',2,min_lod=1),setter(90,2,[910])],'warp',expect_failure=True)

    cases=[('linear',{},dict(filter=21)),('point',dict(uv=(.75,.75)),dict(filter=0)),
        ('minlod',{},dict(filter=21,min_lod=1)),('maxlod',dict(level=1),dict(filter=21,max_lod=0)),
        ('compare-less',dict(comparison=True,uv=(.75,.75)),dict(filter=128,comparison_func=2)),
        ('compare-greater',dict(comparison=True,uv=(.75,.75)),dict(filter=128,comparison_func=5)),
        ('min',{},dict(filter=277)),('max',{},dict(filter=405)),
        ('bias',dict(pixel=True),dict(filter=0,address_u=1,address_v=1,mip_lod_bias=1))]
    cases += [('address'+str(n),dict(uv=(1.25,.25)),dict(filter=0,address_u=n,address_v=n,border_color=[.125]*4)) for n in range(1,6)]
    for name,fixture_args,values in cases:
        path=sample_fixture(a.out/(name+'.gpa_frame'),**fixture_args)
        for driver in ('hardware','warp'):
            pixel=fixture_args.get('pixel',False)
            compare('sampling-'+name,path,[sampler('ps' if pixel else 'cs',0,**values)],driver,resource=40 if pixel else 30,buffer=not pixel,expect_failure=name in ('min','max'))

    path=paths['ps',False]
    for filter_value in sorted(FILTERS):
        for driver in ('hardware','warp'):
            compare('filter'+str(filter_value),path,[sampler('ps',2,filter=filter_value,max_anisotropy=16,mip_lod_bias=-2.5,address_w=5,min_lod=-3,max_lod=7)],driver,expect_failure=bool(filter_value&256))

    invalid=[{},dict(filter=True),dict(filter=2),dict(address_u=0),dict(address_v=6),dict(filter=85,max_anisotropy=0),dict(max_anisotropy=17),dict(comparison_func=9),dict(border_color=[0,0,0]),dict(border_color=[2,0,0,0]),dict(mip_lod_bias=16),dict(min_lod=2,max_lod=1),dict(unknown=0)]
    for n,value in enumerate(invalid):compare('invalid-descriptor'+str(n),path,[sampler('ps',2,**value)],'warp',expect_failure=True)
    for n,op in enumerate([setter(90,16,[]),setter(90,15,[910,911]),setter(90,0,[902]),setter(90,-1,[]),setter(90,0,[True]),sampler('bad',0,filter=0),sampler('ps',True,filter=0),sampler('ps',16,filter=0)]):
        compare('invalid-target'+str(n),path,[op],'warp',expect_failure=True)

if a.captures:
    from events import draw_events
    from state import decode_state
    from sampler_setters import TYPES,captured
    import hashlib
    for tag,filename,golden in [('gf2','GF2_Exilium_2026_03_03__00_19_35.gpa_frame','2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1'),('bf1','bf1_2026_01_21__16_53_05.gpa_frame','1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6')]:
        path=a.captures/filename
        with Frame(path) as f:
            descriptors=[];setters=[]
            for ev in draw_events(f):
                state=decode_state(f.payload(ev['state_id']))
                for stage in ('vs','hs','ds','gs','ps','cs'):
                    if state[stage]['shader']:
                        for slot,rid in enumerate(state[stage]['samplers']):
                            if rid:descriptors.append(dict(kind='sampler',event=ev['id'],stage=stage,slot=slot,values=dict(min_lod=10,max_lod=10)))
            for entry in f.entries.values():
                if entry.category==7 and entry.type in TYPES:
                    v=captured(f.payload(entry.id));setters.append(setter(entry.id,v['start_slot'],[0]*len(v['samplers'])))
            for label,ops in [('lod',descriptors),('null-setters',setters),('original',[])]:
                name='real-'+tag+'-'+label;project=a.out/(name+'.json');exp=experiment(f,project,ops)
                d=create_device('hardware')
                try:expected=Engine(f,d,experiment=exp).replay()[2]
                finally:d.close()
                out,report=native(name,path,'replay','hardware',0,project=project)
                raw=(out/'frame.rgba').read_bytes();h=hashlib.sha256(raw).hexdigest()
                check(name,raw==expected and (h==golden if label=='original' else h!=golden),sha256=h,operations=len(ops))
