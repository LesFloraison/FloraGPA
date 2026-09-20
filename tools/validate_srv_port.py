"""Development-only SRV descriptor comparisons against preserved Python."""
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

if not a.real_only:
    stages={stage:fixture(a.out/(stage+'.gpa_frame'),stage) for stage in ('vs','hs','ds','gs','ps','cs')}
    for stage,path in stages.items():
        for driver in ('hardware','warp'):
            kw=dict(buffer=stage=='cs',resource=74 if stage=='cs' else 30)
            ops=[edit(stage,0,dict(most_detailed_mip=1,mip_levels=1))]
            for event,before in [(100,False),(100,True),(200,False)]:compare(stage,path,ops,driver,event,before,**kw)
            compare(stage+'-disabled',path,ops+[dict(kind='enabled',event=100,value=False)],driver,**kw)
            compare(stage+'-undo',path,[],driver,**kw)
    dimensions={}
    for dim in (1,2,3,4,5,8,9,10,11):
        path,patch,_=dimension_fixture(a.out/f'dimension{dim}.gpa_frame',dim);dimensions[dim]=path
        for driver in ('hardware','warp'):
            compare('dimension'+str(dim),path,[edit('cs',0,patch)],driver)
            compare('dimension'+str(dim)+'-original',path,[],driver)
    for kind in ('structured','format','ms','msarray'):
        path,patch=extra_fixture(a.out/(kind+'.gpa_frame'),kind)
        for driver in ('hardware','warp'):
            compare(kind,path,[edit('cs',0,patch)],driver)
            compare(kind+'-original',path,[],driver)
    path=dimensions[4]
    for driver in ('hardware','warp'):
        compare('remaining-mips',path,[edit('cs',0,dict(most_detailed_mip=1,mip_levels=0xffffffff))],driver)
        # Dimension changes retain common fields and drop fields from old unions.
        compare('dimension-change',path,[edit('cs',0,dict(dimension=5,first_array_slice=0,array_size=1)),edit('cs',0,dict(dimension=4,most_detailed_mip=1,mip_levels=1))],driver)
        for i,values in enumerate((dict(format=28),dict(most_detailed_mip=2),dict(mip_levels=3),dict(dimension=8))):
            compare('invalid-native'+str(i),path,[edit('cs',0,values)],driver,fail=True)
    code=compile_hlsl('Texture2D<float> a:register(t0);Texture2D<float> b:register(t1);RWBuffer<float> dst:register(u0);[numthreads(1,1,1)]void main(){dst[0]=a.Load(int3(0,0,0))+b.Load(int3(0,0,0));}','cs_5_0')[0]
    def alias(entry,raw):
        if entry.id==11:return struct.pack('<Q',len(code))+code+bytes(8)
        if entry.category==3:
            raw=bytearray(raw);struct.pack_into('<Q',raw,17788,22);struct.pack_into('<Q',raw,17780+127*8,22)
        return raw
    alias_path=clone(path,'alias',alias)
    for driver in ('hardware','warp'):
        compare('alias',alias_path,[edit('cs',1,dict(most_detailed_mip=1,mip_levels=1)),edit('cs',127,dict(most_detailed_mip=1,mip_levels=1))],driver)
    for dim in (1,11):
        data=a.out/f'buffer{dim}.bin';data.write_bytes(struct.pack('<f',.875))
        ops=[dict(kind='buffer',event=100,resource=20,offset=16,asset=blob(data)),edit('cs',0,dict(first_element=4,num_elements=4))]
        for driver in ('hardware','warp'):
            compare('buffer-clone'+str(dim),dimensions[dim],ops,driver)
            compare('buffer-clone-original'+str(dim),dimensions[dim],ops,driver,resource=20)
    x=Fixture()
    x.add(20,5,0x85,bytes(16)+struct.pack('<11IQ',2,2,2,1,41,1,0,0,136,0,0,21))
    x.data(21,struct.pack('<5f',.125,.125,.125,.125,.75))
    x.view(22,'srv',20,[41,4,0,1,0,0]);x.view(24,'uav',20,[41,4,1,0,0])
    x.buffer(30,[4,0,128,0,0,0],bytes(4));x.view(32,'uav',30,[41,1,0,1,0])
    x.shader(10,'cs','Texture2D<float> src:register(t0);RWTexture2D<float> dst:register(u0);RWBuffer<float> observed:register(u1);[numthreads(1,1,1)]void main(){float v=src.Load(int3(0,0,0))+.25;dst[uint2(0,0)]=v;observed[0]=v;}')
    x.events(state_bytes([(17772,10),(17780,22),(20864,24),(20872,32)]),0x35,struct.pack('<III',1,1,1))
    hazard=x.write(a.out/'hazard.gpa_frame')
    for driver in ('hardware','warp'):
        compare('distinct-mips',hazard,[],driver)
        compare('overlapping-mips',hazard,[edit('cs',0,dict(most_detailed_mip=1))],driver)
    bad=[{},dict(format=True),dict(bad=1),dict(dimension=0),dict(dimension=12),dict(first_element=-1),dict(mip_levels=0),dict(flags=2),dict(num_cubes=0),dict(array_size=2**32)]
    for i,value in enumerate(bad):compare('invalid-fields'+str(i),path,[edit('cs',0,value)],'warp',fail=True)
    for i,(stage,slot,value) in enumerate([('bad',0,dict(format=41)),('cs',True,dict(format=41)),('cs',128,dict(format=41)),('cs',1,dict(format=41)),('cs',0,dict(first_element=1)),('cs',0,dict(dimension=5))]):
        compare('invalid-target'+str(i),path,[edit(stage,slot,value)],'warp',fail=True)

if a.captures:
    from events import draw_events
    from state import decode_state
    from srv_edits import descriptor
    import hashlib
    for tag,filename,golden in [('gf2','GF2_Exilium_2026_03_03__00_19_35.gpa_frame','2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1'),('bf1','bf1_2026_01_21__16_53_05.gpa_frame','1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6')]:
        path=a.captures/filename
        with Frame(path) as f:
            operations=[]
            for ev in draw_events(f):
                state=decode_state(f.payload(ev['state_id']))
                for stage in ('vs','hs','ds','gs','ps','cs'):
                    if not state[stage]['shader']:continue
                    for slot,rid in enumerate(state[stage]['srv']):
                        if not rid or rid not in f.entries:continue
                        desc=descriptor(f,rid)
                        if desc.get('mip_levels',0) not in (0,1,0xffffffff):operations.append(edit(stage,slot,dict(most_detailed_mip=desc['most_detailed_mip']+1,mip_levels=desc['mip_levels']-1),ev['id']))
            assert operations
            for label,ops in [('mips',operations),('original',[])]:
                name='real-'+tag+'-'+label;project=a.out/(name+'.json');exp=experiment(f,project,ops);d=create_device('hardware')
                try:expected=Engine(f,d,experiment=exp).replay()[2]
                finally:d.close()
                out,_=native(name,path,'replay','hardware',0,project=project)
                raw=(out/'frame.rgba').read_bytes();h=hashlib.sha256(raw).hexdigest()
                check(name,raw==expected and (h==golden if label=='original' else h!=golden),sha256=h,operations=len(ops))
