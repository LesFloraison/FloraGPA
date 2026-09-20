"""Development-only predicate setter comparisons against preserved Python."""
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
def compare(name,path,driver,event=300,before=False,project=None,pixels=True,buffer=True,pipeline=False):
    name+='-'+driver+'-'+str(event)+('-before' if before else '')
    with Frame(path) as f:
        d=create_device(driver)
        try:
            e=Engine(f,d,experiment=Experiment(f,project) if project else None)
            e.replay(until=event,before=before,readback=False)
            expected=inspect_result(e,60);expected.update(event=event,value_time='before_command' if before else 'after_command')
            image=e.read_texture(30) if pixels else None
            raw=e.read_buffer(82) if buffer else None
            history=e.so_count_history
            if pipeline:state=inspect_pipeline(Engine(f,d,experiment=Experiment(f,project) if project else None),event,after=not before)
        finally:d.close()
    out,report=native(name+'-predicate',path,'predicate',driver,event,before,60,project)
    actual=json.loads((out/'predicate.json').read_text())
    check(name+'-predicate',actual==expected,status=actual['status'])
    actual_history=[{k:int(v) for k,v in row.items()} for row in report['stream_output_history']]
    check(name+'-so-history',actual_history==history)
    if pixels:
        out,_=native(name+'-image',path,'replay',driver,event,before,30,project)
        check(name+'-image',(out/'frame.rgba').read_bytes()==image)
        out,_=native(name+'-preview',path,'texture',driver,event,before,30,project)
        check(name+'-preview',(out/'frame.rgba').read_bytes()==image)
    if buffer:
        out,_=native(name+'-buffer',path,'buffer',driver,event,before,82,project)
        check(name+'-buffer',(out/'buffer.bin').read_bytes()==raw)
    if pipeline:
        out,_=native(name+'-pipeline',path,'replay-pipeline',driver,event,before,project=project)
        actual=json.loads((out/'replay-pipeline.json').read_text())
        check(name+'-pipeline',actual['fields']==state['fields'] and actual['limits']==state['limits'])

from validate_predicate_setters import fixture as setter_fixture,VERSIONS
for version in VERSIONS:
    for visible in (False,True):
        name=f'{version[0]}-{int(visible)}'
        path=setter_fixture(a.out/(name+'.gpa_frame'),version,visible)
        for label,rid,value in [('invert',60,1),('replace',62,0),('overflow',64,1),('detach',0,0xffffffff),('raw',60,0xffffffff)]:
            project=a.out/(name+'-'+label+'.json')
            with Frame(path) as f:experiment(f,project,[dict(kind='setter',event=120,values=dict(predicate=rid,predicate_value=value))])
            for driver in ('hardware','warp'):
                compare(name+'-'+label,path,driver,300,project=project,pipeline=True)
                if version==VERSIONS[-1]:
                    for event,before in [(120,True),(120,False),(140,False),(200,True)]:
                        compare(name+'-'+label,path,driver,event,before,project=project,pipeline=True)
for later in ('setter','clear'):
    path=setter_fixture(a.out/('later-'+later+'.gpa_frame'),VERSIONS[-1],True,later)
    project=path.with_suffix('.json')
    with Frame(path) as f:experiment(f,project,[dict(kind='setter',event=120,values=dict(predicate=62,predicate_value=0))])
    for driver in ('hardware','warp'):compare('later-'+later,path,driver,project=project,pipeline=True)
path=setter_fixture(a.out/'invalid.gpa_frame',VERSIONS[-1],True)
invalid=[dict(predicate=True,predicate_value=0),dict(predicate=-1,predicate_value=0),dict(predicate=82,predicate_value=0),dict(predicate=999999,predicate_value=0),dict(predicate=60,predicate_value=True),dict(predicate=60,predicate_value=-1),dict(predicate=60,predicate_value=2**32),dict(predicate=60),dict(predicate=60,predicate_value=1,extra=1)]
from setter_edits import validate
for n,value in enumerate(invalid):
    project=a.out/f'invalid-{n}.json'
    with Frame(path) as f:
        rejected=False
        try:validate(f,f.entries[120],value)
        except ValueError:rejected=True
        # Write deliberately invalid history without the reference writer validating it.
        document=dict(format='FloraGPA experiment 1',frame_sha256=__import__('hashlib').sha256(path.read_bytes()).hexdigest(),frame_name=path.name,cursor=1,history=[dict(label='invalid',operations=[dict(kind='setter',event=120,values=value)])])
        project.write_text(json.dumps(document))
    native('invalid-'+str(n),path,'predicate','warp',300,resource=60,project=project,fail=True)
    check('invalid-'+str(n),rejected)
# Edited query lifecycle, including original unbind cancelling an override.
for case in ('active','bound','unbind'):
    def change(e,raw):return None if case=='active' and e.id==110 else raw
    def extras(x):
        if case in ('bound','unbind'):
            x.add(160,7,0x34f9,struct.pack('<QQQ',0,1,62));x.add(170,7,0x34fa,struct.pack('<QQQ',0,1,62))
        if case=='unbind':x.add(150,7,0x34fc,struct.pack('<QQQI',0,1,0,0))
    target=clone(path,'lifecycle-'+case,change,extras)
    project=target.with_suffix('.json')
    with Frame(target) as f:experiment(f,project,[dict(kind='setter',event=120,values=dict(predicate=60 if case=='active' else 62,predicate_value=1))])
    for driver in ('hardware','warp'):
        if case=='unbind':compare('lifecycle-'+case,target,driver,170,project=project,pipeline=True)
        else:
            event=120 if case=='active' else 160
            with Frame(target) as f:
                d=create_device(driver);rejected=False
                try:
                    try:Engine(f,d,experiment=Experiment(f,project)).replay(event,readback=False)
                    except ValueError:rejected=True
                finally:d.close()
            native('lifecycle-'+case+'-'+driver,target,'predicate',driver,event,resource=60,project=project,fail=True)
            check('lifecycle-'+case+'-'+driver,rejected)
# Predicate edits change actual SO production and downstream DrawAuto counts.
from validate_so_history import fixture as so_fixture
seed=so_fixture(a.out/'so-seed.gpa_frame')
def so_change(e,raw):
    if e.id in (149,199):raw=bytearray(raw);struct.pack_into('<QI',raw,20960,60,0)
    return raw
def so_extras(x):
    x.add(60,5,0x96,struct.pack('<QQII',0,0,5,0));x.add(120,7,0x34fc,struct.pack('<QQQI',0,1,60,0))
target=clone(seed,'so-edited',so_change,so_extras)
for bit in (0,1):
    project=a.out/f'so-edit-{bit}.json'
    with Frame(target) as f:experiment(f,project,[dict(kind='setter',event=120,values=dict(predicate=60,predicate_value=bit))])
    for driver in ('hardware','warp'):
        compare('so-edit-'+str(bit),target,driver,200,project=project,pixels=False,buffer=False,pipeline=True)
        out,_=native('so-geometry-'+str(bit)+'-'+driver,target,'geometry',driver,200,project=project)
        geometry=__import__('json').loads((out/'geometry.json').read_text())
        check('so-count-'+str(bit)+'-'+driver,geometry['vertex_references']==(6 if bit else 3))
print(len(checks),'predicate setter comparisons PASS',flush=True)
