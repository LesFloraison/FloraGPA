"""Development-only predicate replay/inspection comparisons against preserved Python."""
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

for visible in (False,True):
    for value in (0,1,7,0xffffffff):
        name=f'base-{int(visible)}-{value}'
        path=fixture(a.out/(name+'.gpa_frame'),visible,value)
        for driver in ('hardware','warp'):
            compare(name,path,driver,pipeline=True)
            for event in (90,100,110,120,140,200):
                compare(name,path,driver,event,pixels=event>=120,buffer=event>=120)
            compare(name,path,driver,110,True,pixels=False,buffer=False)

base=a.out/'base-1-0.gpa_frame'
for version,types in enumerate([(0x30b2,0x30b3,0x30b5),(0x31b2,0x31b3,0x31b5),(0x331b,0x331c,0x331e),(0x33e1,0x33e2,0x33e4),(0x34f9,0x34fa,0x34fc)]):
    x=Fixture();x.records=[]
    with Frame(base) as f:
        for e in f.entries.values():x.add(e.id,e.category,dict(zip((0x241,0x243,0x248),types)).get(e.type,e.type),f.payload(e.id))
    path=a.out/f'version-{version}.gpa_frame';x.write(path)
    for driver in ('hardware','warp'):compare('version-'+str(version),path,driver)

for value in (0,1):
    path=fixture(a.out/f'seed-{value}.gpa_frame',False,value,seed_only=True)
    for driver in ('hardware','warp'):compare('seed-'+str(value),path,driver)

def hint(e,raw):
    if e.id==60:raw=raw[:20]+struct.pack('<I',1)
    return raw
path=clone(base,'hint',hint)
for driver in ('hardware','warp'):compare('hint',path,driver)

path=clone(a.out/'base-1-1.gpa_frame','clear',extras=lambda x:x.add(125,7,0x242,struct.pack('<QQ',0,1)))
for driver in ('hardware','warp'):compare('clear',path,driver)

code=a.out/'discard.dxbc';code.write_bytes(compile_hlsl('float4 main():SV_Target{discard;return 0;}','ps_5_0')[0])
for name,path,operations in [
    ('discard',base,[dict(kind='shader',resource=12,asset=blob(code))]),
    ('disabled-producer',base,[dict(kind='enabled',event=100,value=False)]),
    ('skipped-output-edit',a.out/'base-1-1.gpa_frame',[operation(82,0,struct.pack('<I',21),300)])]:
    project=a.out/(name+'.json')
    with Frame(path) as f:experiment(f,project,operations)
    for driver in ('hardware','warp'):compare(name,path,driver,project=project)

multi=a.out/'so.gpa_frame';multi_fixture(multi,0xffffffff)
for slot in range(4):
    for overflow in (False,True):
        def change(e,raw):
            if overflow and e.id==40+2*slot:raw=raw[:16]+struct.pack('<I',64)+raw[20:]
            if overflow and e.id==41+2*slot:raw=struct.pack('<I',64)+bytes([0xcd])*64
            return raw
        def extras(x):
            x.add(60,5,0x96,struct.pack('<QQII',0,0,7,0))
            x.add(90,7,0x241,struct.pack('<QQQ',0,1,60));x.add(160,7,0x243,struct.pack('<QQQ',0,1,60))
        name=f'overflow-{slot}-{int(overflow)}';path=clone(multi,name,change,extras)
        for driver in ('hardware','warp'):compare(name,path,driver,160,pixels=False,buffer=False)

for name,change,extras,event in [
    ('double-begin',lambda e,r:r,lambda x:x.add(95,7,0x241,struct.pack('<QQQ',0,1,60)),100),
    ('end-without-begin',lambda e,r:None if e.id==90 else r,lambda x:None,110),
    ('bind-active',lambda e,r:r,lambda x:x.add(95,7,0x248,struct.pack('<QQQI',0,1,60,0)),100),
    ('bound-begin',lambda e,r:r,lambda x:x.add(125,7,0x241,struct.pack('<QQQ',0,1,60)),130),
    ('bad-type',lambda e,r:r[:16]+struct.pack('<II',1,0) if e.id==60 else r,lambda x:None,90),
    ('bad-flags',lambda e,r:r[:20]+struct.pack('<I',2) if e.id==60 else r,lambda x:None,90),
    ('truncated-begin',lambda e,r:r[:-1] if e.id==90 else r,lambda x:None,90),
    ('null-begin',lambda e,r:r[:16]+bytes(8) if e.id==90 else r,lambda x:None,90)]:
    path=clone(base,name,change,extras)
    with Frame(path) as f:
        d=create_device('warp');rejected=False
        try:
            try:Engine(f,d).replay(until=event,readback=False)
            except (ValueError,RuntimeError):rejected=True
        finally:d.close()
    native(name,path,'predicate','warp',event,resource=60,fail=True);check(name,rejected)
print(len(checks),'predicate comparisons PASS',flush=True)
