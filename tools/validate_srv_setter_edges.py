"""Development-only SRV setter and descriptor inheritance comparisons."""
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
    if value and 'expected' in detail and 'actual' in detail:
        import hashlib
        detail={k+'_sha256':hashlib.sha256(json.dumps(v,sort_keys=True).encode()).hexdigest() for k,v in detail.items()}
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

from validate_srv_descriptors import fixture,dimension_fixture,extra_fixture,edit
from validate_buffer_edits import state_bytes

def compare(name,path,ops,driver,event=100,before=False,resource=30,buffer=True,fail=False,pixels=True):
    project=a.out/(name+'.json');error=None
    with Frame(path) as f:
        try:
            exp=experiment(f,project,ops);d=create_device(driver)
            try:
                engine=Engine(f,d,experiment=exp);engine.replay(event,before=before,readback=False)
                expected=(engine.read_buffer(resource) if buffer else engine.read_texture(resource)) if pixels else None
                state=inspect_pipeline(Engine(f,d,experiment=exp),event,after=not before)
            finally:d.close()
        except (ValueError,RuntimeError,KeyError) as ex:
            if not fail:raise
            error=str(ex)
            if not project.exists():project.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=__import__('hashlib').sha256(path.read_bytes()).hexdigest(),frame_name=path.name,cursor=1,history=[dict(label='invalid',operations=ops)])))
    label=name+'-'+driver+'-'+str(event)+('-before' if before else '')
    if fail:
        assert error,(name,'reference unexpectedly accepted')
        native(label,path,'replay-pipeline',driver,event,before,project=project,fail=True);check(label,True,reference_error=error);return
    out,report=native(label+'-pipeline',path,'replay-pipeline',driver,event,before,project=project)
    actual=json.loads((out/'replay-pipeline.json').read_text())
    def select(result):
        rows=[v for v in result['fields'] if '.srv.' in v['field']];assert len(rows)==768
        result={};groups={}
        for row in rows:
            v=json.loads(json.dumps(row));obj=v.get('object')
            if obj:
                token=str(v['value']);groups.setdefault(token,len(groups)+1);v['value']=groups[token]
                for key in ('token','runtime_token'):obj.pop(key,None)
                obj['runtime_object']='view-'+str(groups[token])
            result[v['field']]=v
        return result
    wanted=select(state);observed=select(actual)
    check(label+'-views',observed==wanted,expected=wanted,actual=observed)
    if pixels:
        out,_=native(label+'-output',path,'buffer' if buffer else 'texture',driver,event,before,resource,project)
        check(label+'-output',(out/('buffer.bin' if buffer else 'frame.rgba')).read_bytes()==expected)

from validate_srv_setters import fixture as setter_fixture

def setter(event,start,ids):return dict(kind='setter',event=event,values=dict(start_slot=start,views=ids))
ops=[setter(90,2,[902,912]),setter(95,127,[922])]

base=setter_fixture(a.out/'ps.gpa_frame','ps')
def alternate_context(en,raw):
    if en.id==90:
        raw=bytearray(raw);struct.pack_into('<Q',raw,8,7777)
    return raw
context_path=clone(base,'context-record-order',alternate_context,lambda x:x.add(7777,5,0x127,bytes(24)))
for driver in ('hardware','warp'):
    for reverse in (False,True):
        operations=[ops[0],edit('ps',2,dict(mip_levels=1))]
        if reverse:operations.reverse()
        compare('context-record-order-'+str(reverse),context_path,operations,driver,resource=30,buffer=False)
for writable in (False,True):
    def change(en,raw):
        if writable and en.id==900:
            raw=bytearray(raw);struct.pack_into('<I',raw,48,168)
        return raw
    path=clone(base,'gap-'+str(writable),change,lambda x:x.add(88,7,0x34ff,__import__('validate_output_commands').rt([999999],0)))
    for driver in ('hardware','warp'):
        compare('gap-boundary-'+str(writable),path,ops,driver,95,fail=True,pixels=False)
        compare('gap-snapshot-'+str(writable),path,ops,driver,100,resource=30,buffer=False,fail=writable)
future=clone(base,'future-invalid',lambda en,raw:struct.pack('<QQIIB',0,1,3,1,0) if en.id==110 else raw)
for driver in ('hardware','warp'):
    compare('future-early',future,ops,driver,100,resource=30,buffer=False)
    compare('future-reached',future,ops,driver,120,fail=True,pixels=False)
    for i,values in enumerate([dict(start_slot=128,views=[]),dict(start_slot=127,views=[902,912]),dict(start_slot=0,views=[True]),dict(start_slot=0,views=[900]),dict(start_slot=0,views=[905]),dict(start_slot=0,views=[999999])]):
        compare('invalid-'+str(i),base,[dict(kind='setter',event=90,values=values)],driver,fail=True,pixels=False)
for dim in range(1,12):
    if dim in (6,7):path,patch=extra_fixture(a.out/f'dim{dim}.base','ms' if dim==6 else 'msarray')
    else:path,patch,_=dimension_fixture(a.out/f'dim{dim}.base',dim)
    def change(en,raw):
        if en.category==3:
            raw=bytearray(raw);struct.pack_into('<Q',raw,17780,0)
        return raw
    def commands(x):
        x.add(70,7,0x242,struct.pack('<QQ',0,1));x.add(80,7,0x3521,struct.pack('<QQIIBQ',0,1,0,1,1,0))
    path=clone(path,f'dim{dim}',change,commands)
    for driver in ('hardware','warp'):
        for reverse in (False,True):
            operations=[setter(80,0,[22]),edit('cs',0,patch)]
            if reverse:operations.reverse()
            compare(f'inherited-dim{dim}-order{reverse}',path,operations,driver)
        if dim in (1,11):
            data=a.out/'buffer-patch.bin';data.write_bytes(struct.pack('<f',.5))
            buffer_patch=dict(kind='buffer',event=100,resource=20,offset=16,asset=blob(data))
            operations=[setter(80,0,[22]),edit('cs',0,dict(first_element=4,num_elements=4)),buffer_patch]
            compare(f'clone-dim{dim}',path,operations,driver)
            compare(f'clone-dim{dim}-reversed',path,list(reversed(operations)),driver)
            for after in (False,True):
                name=f'clone-storage-{dim}-{driver}-{after}'
                project=a.out/(name+'.json');expected_dir=a.out/(name+'-python');expected_dir.mkdir()
                with Frame(path) as f:
                    exp=experiment(f,project,operations);d=create_device(driver)
                    try:
                        from buffers import export_buffer
                        expected=export_buffer(Engine(f,d,experiment=exp),20,expected_dir,100,after=after)
                    finally:d.close()
                out,report=native(name,path,'buffer',driver,100,before=not after,resource=20,project=project)
                actual_bindings=[{k:int(v) if k=='view' else v for k,v in row.items()} for row in report['bindings']]
                check(name,(out/'buffer.bin').read_bytes()==(expected_dir/'buffer.bin').read_bytes() and actual_bindings==expected['bindings'] and report['edit_effect']==expected['edit_effect'])
for plane,fmt in [('depth',46),('stencil',47)]:
    for flags in range(4):
        x=Fixture()
        x.add(20,5,0x85,bytes(16)+struct.pack('<11IQ',1,1,1,1,44,1,0,0,72,0,0,21));x.data(21,struct.pack('<I',0x55800000))
        x.view(22,'srv',20,[fmt,4,0,1,0,0]);x.add(24,5,0x8e,bytes(16)+struct.pack('<Q6I',20,45,3,flags,0,0,0))
        x.add(70,7,0x242,struct.pack('<QQ',0,1))
        x.add(75,7,0x34ff,__import__('validate_output_commands').rt([],24))
        x.add(80,7,0x3521,struct.pack('<QQIIBQ',0,1,0,1,1,0));x.add(90,7,0x244,struct.pack('<QQ',0,1))
        path=x.write(a.out/f'{plane}-{flags}.gpa_frame')
        for driver in ('hardware','warp'):
            compare(f'readonly-{plane}-{flags}',path,[setter(80,0,[22])],driver,90,pixels=False)
print('Completed',len(checks),'checks',flush=True)
