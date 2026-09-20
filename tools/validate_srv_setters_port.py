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
    # Keep full fields on a mismatch; successful native states already have per-case artifacts.
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
if not a.real_only:
    for stage in ('vs','hs','ds','gs','ps','cs'):
        for clear in (False,True):
            tag=stage+('-clear' if clear else '')
            path=setter_fixture(a.out/(tag+'.gpa_frame'),stage,clear=clear)
            for driver in ('hardware','warp'):
                kw=dict(resource=74 if stage=='cs' else 30,buffer=stage=='cs')
                for event,before in [(95,False),(100,True),(100,False),(110,False),(120,False),(130,False),(140,False),(200,False),(300,False)]:
                    compare(tag,path,ops,driver,event,before,pixels=event>=100,**kw)
                compare(tag+'-disabled',path,ops+[dict(kind='enabled',event=100,value=False)],driver,**kw)
                compare(tag+'-undo',path,[],driver,**kw)
                if not clear:
                    for label,start,ids in [('shrink',3,[912]),('move',4,[912]),('empty',2,[])]:
                        compare(tag+'-'+label,path,[setter(90,start,ids),ops[1]],driver,**kw)
                    compare(tag+'-unused',path,[ops[0],setter(95,126,[922])],driver,**kw)
                    for order in (False,True):
                        descriptor=edit(stage,2,dict(mip_levels=1))
                        compare(tag+'-inherit-'+str(order),path,([descriptor]+ops if order else ops+[descriptor]),driver,**kw)
        for hazard in ('cs-first','cs-last','rt-first','rt-last'):
            path=setter_fixture(a.out/(stage+'-'+hazard+'.gpa_frame'),stage,hazard=hazard)
            for driver in ('hardware','warp'):
                for event in (95,100,106,200):
                    compare(stage+'-'+hazard,path,ops,driver,event,resource=74 if stage=='cs' else 30,buffer=stage=='cs',pixels=event>=100)
        for missing in (True,'clear','unresolved'):
            path=setter_fixture(a.out/(stage+'-missing-'+str(missing)+'.gpa_frame'),stage,missing=missing)
            for driver in ('hardware','warp'):
                compare(stage+'-missing-'+str(missing),path,ops,driver,98 if missing is True else 97 if missing=='clear' else 96,resource=74 if stage=='cs' else 30,buffer=stage=='cs',fail=missing=='unresolved',pixels=False)
                compare(stage+'-missing-snapshot-'+str(missing),path,ops,driver,100,resource=74 if stage=='cs' else 30,buffer=stage=='cs')
        path=setter_fixture(a.out/(stage+'-unknown.gpa_frame'),stage,known=False)
        for driver in ('hardware','warp'):
            compare(stage+'-unknown-move',path,[setter(90,4,[912])],driver,fail=True,pixels=False)
print('Completed',len(checks),'checks',flush=True)

if a.captures:
    import hashlib
    from srv_setters import TYPES,captured
    for tag,filename,golden in [('gf2','GF2_Exilium_2026_03_03__00_19_35.gpa_frame','2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1'),('bf1','bf1_2026_01_21__16_53_05.gpa_frame','1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6')]:
        path=a.captures/filename
        with Frame(path) as f:
            operations=[]
            for en in f.entries.values():
                if en.category==7 and en.type in TYPES:
                    value=captured(f.payload(en.id))
                    if value['views']:operations.append(setter(en.id,value['start_slot'],[0]*len(value['views'])))
            assert operations
            for label,operations in [('null',operations),('original',[])]:
                name='real-'+tag+'-'+label;project=a.out/(name+'.json');exp=experiment(f,project,operations);d=create_device('hardware')
                try:expected=Engine(f,d,experiment=exp).replay()[2]
                finally:d.close()
                out,_=native(name,path,'replay','hardware',0,project=project)
                raw=(out/'frame.rgba').read_bytes();h=hashlib.sha256(raw).hexdigest()
                check(name,raw==expected and (h==golden if label=='original' else h!=golden),sha256=h,operations=len(operations))
