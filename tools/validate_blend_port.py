"""Development-only blend pipeline edit comparisons against preserved Python."""
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

from validate_blend_edits import fixture as blend_fixture
from validate_buffer_edits import compute_fixture
from assets import gpu_preview
from rasterizer_edits import normalize

def compare(name,path,operations,driver,event=100,before=False,targets=(1000,),expect_failure=False):
    project=a.out/(name+'.json');error=None
    with Frame(path) as f:
        try:
            experiment(f,project,operations)
            d=create_device(driver)
            try:
                exp=Experiment(f,project);e=Engine(f,d,experiment=exp)
                e.replay(event,before=before,readback=False)
                pixels={id:gpu_preview(f.resource(id),e.read_texture(id),driver=driver)[2] for id in targets}
                state=inspect_pipeline(Engine(f,d,experiment=exp),event,after=not before)
            finally:d.close()
        except (ValueError,RuntimeError) as ex:
            if not expect_failure:raise
            error=str(ex)
            if not project.exists():
                project.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=__import__('hashlib').sha256(path.read_bytes()).hexdigest(),frame_name=path.name,cursor=1,history=[dict(label='invalid',operations=operations)])))
    label=name+'-'+driver+'-'+str(event)+('-before' if before else '')
    if expect_failure:
        assert error,(name,'reference unexpectedly accepted')
        native(label,path,'replay-pipeline',driver,event,before,project=project,fail=True)
        check(label,True,reference_error=error);return
    out,report=native(label+'-pipeline',path,'replay-pipeline',driver,event,before,project=project)
    actual=json.loads((out/'replay-pipeline.json').read_text())
    select=lambda result:{v['field']:v for v in result['fields'] if v['field'] in ('blend.descriptor','sample_mask','rasterizer.descriptor','depth_state.descriptor','stencil_ref','viewports','scissors') or v['field'].startswith('blend_factor.')}
    expected=select(state);observed=select(actual)
    assert all(k in expected for k in ('blend.descriptor','blend_factor.0','blend_factor.3','sample_mask'))
    check(label+'-pipeline',observed==expected,expected=expected,actual=observed)
    check(label+'-draws',actual['counts'].get('Draw',0)==state['counts'].get('Draw',0))
    for id,rgba in pixels.items():
        out,_=native(label+'-texture'+str(id),path,'texture',driver,event,before,id,project)
        check(label+'-texture'+str(id),(out/'frame.rgba').read_bytes()==rgba)
    return pixels

def values(row,**extra):return dict(blend_state=dict(targets={'0':row}),**extra)
def ops(value):return [dict(kind='pipeline',event=100,values=value)]
paths={name:blend_fixture(a.out/(name+'.gpa_frame'),**opts) for name,opts in [('single',{}),('dual',dict(dual=True)),('mrt',dict(targets=8)),('logic',dict(logical=True)),('msaa',dict(samples=4)),('captured-logic',dict(logical=True,captured_logic=10)),('captured-bad-logic',dict(captured_logic=10))]}
for driver in ('hardware','warp'):
    for kind in ('single','dual'):
        factors=range(16,20) if kind=='dual' else list(range(1,12))+[14,15]
        for n in factors:
            for side in ('src','dest'):
                compare(kind+'-'+side+str(n),paths[kind],ops(values(dict(blend_enable=True,src_blend=n if side=='src' else 1,dest_blend=n if side=='dest' else 1))),driver)
            if n not in (3,4,9,10,16,17):
                compare(kind+'-alpha'+str(n),paths[kind],ops(values(dict(blend_enable=True,src_blend_alpha=n))),driver)
    for op in range(1,6):
        compare('operation'+str(op),paths['single'],ops(values(dict(blend_enable=True,src_blend=2,dest_blend=2,blend_op=op,src_blend_alpha=2,dest_blend_alpha=2,blend_op_alpha=op))),driver)
    for mask in range(16):
        compare('write-mask'+str(mask),paths['single'],ops(values(dict(write_mask=mask))),driver)
    for op in range(16):
        compare('logic'+str(op),paths['logic'],ops(values(dict(logic_op_enable=True,logic_op=op))),driver)
    compare('captured-logic',paths['captured-logic'],[],driver)
    targets={str(i):dict(blend_enable=True,src_blend=2,dest_blend=2,blend_op=1+i%5,write_mask=i+1) for i in range(8)}
    compare('mrt',paths['mrt'],ops(dict(blend_state=dict(independent_blend=True,targets=targets),blend_factor=[.5,.25,1,.75],sample_mask=0xf5f5f5f5)),driver,targets=tuple(1000+4*i for i in range(8)))
    compare('inactive-logic',paths['mrt'],ops(dict(blend_state=dict(targets={'7':dict(logic_op_enable=True)}))),driver,targets=(1000,1028))
    for mask in (0,1,5,15,0xffffffff):
        compare('sample-mask'+str(mask),paths['msaa'],ops(dict(sample_mask=mask)),driver,targets=())
    compare('alpha-coverage',paths['msaa'],ops(dict(blend_state=dict(alpha_to_coverage=True))),driver,targets=())
    compare('blend-constant',paths['single'],ops(values(dict(blend_enable=True,src_blend=14,src_blend_alpha=14),blend_factor=[.5,.25,1,.75])),driver)
    edits=[dict(kind='pipeline',event=100,values=v) for v in (values(dict(blend_enable=True,src_blend=5)),dict(blend_disabled=True),values(dict(dest_blend=6)),dict(blend_disabled=False,sample_mask=0,depth_test=False,rasterizer=dict(cull_mode=1)))]
    for event,before in ((100,True),(100,False),(200,False)):
        compare('history',paths['single'],edits,driver,event,before)
    for before in (False,True):
        compare('bad-format',paths['single'],ops(values(dict(logic_op_enable=True))),driver,before=before,expect_failure=True)
        compare('bad-captured-format',paths['captured-bad-logic'],[],driver,before=before,expect_failure=True)
    compare('bad-disabled-format',paths['single'],ops(values(dict(logic_op_enable=True)))+[dict(kind='enabled',event=100,value=False)],driver,expect_failure=True)
    compare('logic-and-blend',paths['logic'],ops(values(dict(logic_op_enable=True,blend_enable=True))),driver,expect_failure=True)
    compare('independent-logic',paths['logic'],ops(dict(blend_state=dict(independent_blend=True,targets={'0':dict(logic_op_enable=True)}))),driver,expect_failure=True)
    def legacy(e,raw):
        if e.id==64:
            # Keep the independent old 264-byte layout; update the entry type in a fresh fixture below.
            return bytes(16)+struct.pack('<2I',0,0)+struct.pack('<7I4B',0,2,1,1,2,1,1,15,0,0,0)*8
        return raw
    x=Fixture();x.records=[]
    with Frame(paths['single']) as f:
        for e in f.entries.values():x.add(e.id,e.category,0x8a if e.id==64 else e.type,legacy(e,f.payload(e.id)))
    path=a.out/f'{driver}-legacy.gpa_frame';x.write(path)
    compare('legacy',path,ops(values(dict(blend_enable=True,src_blend=5,dest_blend=6))),driver)
    def captured_enabled(e,raw):
        if e.id==64:
            raw=bytearray(raw)
            struct.pack_into('<I',raw,24,1)
        return raw
    enabled=clone(paths['single'],driver+'-captured-enabled',captured_enabled)
    edits=ops(dict(blend_disabled=True))+ops(values(dict(src_blend=5,dest_blend=6)))+ops(dict(blend_disabled=False))
    compare('restore-captured-enable',enabled,edits,driver)
    def captured_booleans(e,raw):
        if e.id==64:
            raw=bytearray(raw)
            struct.pack_into('<I',raw,16,7)
            struct.pack_into('<I',raw,24,2)
        return raw
    noncanonical=clone(paths['single'],driver+'-captured-booleans',captured_booleans)
    compare('captured-booleans',noncanonical,[],driver)
invalid=[{},dict(blend_state={}),dict(blend_state=dict(targets={})),dict(blend_state=dict(targets={'8':dict(write_mask=1)})),dict(blend_state=dict(targets={'0':{}})),dict(blend_state=dict(targets={'00':dict(write_mask=1)})),dict(blend_state=dict(independent_blend=1)),dict(blend_disabled=1),dict(blend_disabled=True,blend_state=dict(alpha_to_coverage=True)),dict(sample_mask=True),dict(sample_mask=-1),dict(sample_mask=2**32),dict(blend_factor=[0,1,2,0]),dict(blend_factor=[0,0,0]),dict(blend_factor=[False,0,0,0]),values(dict(src_blend=12)),values(dict(src_blend_alpha=3)),values(dict(blend_op=0)),values(dict(logic_op=16)),values(dict(write_mask=-1)),values(dict(blend_enable=1)),values(dict(unknown=1))]
for i,value in enumerate(invalid):
    try:normalize(value);rejected=False
    except ValueError:rejected=True
    check('normalize-invalid'+str(i),rejected)
    compare('invalid'+str(i),paths['single'],ops(value),'warp',expect_failure=True)
compute=compute_fixture(a.out/'compute.gpa_frame')
compare('dispatch',compute,ops(dict(sample_mask=0)),'warp',targets=(),expect_failure=True)
if a.captures:
    from events import draw_event
    from state import decode_state
    for label,file,event in [('gf2','GF2_Exilium_2026_03_03__00_19_35.gpa_frame',1455),('bf1','bf1_2026_01_21__16_53_05.gpa_frame',30072)]:
        path=a.captures/file
        with Frame(path) as f:
            state=decode_state(f.payload(draw_event(f,f.entries[event])['state_id']))
            ids=tuple(sorted({struct.unpack_from('<Q',f.payload(view),16)[0] for view in state['rtv'][:min(state['rt_count'],state['om_uav_start'],8)] if view}))
        assert ids,(label,'missing outputs')
        before=compare(label+'-before',path,[],'hardware',event,True,ids)
        original=compare(label+'-original',path,[],'hardware',event,False,ids)
        edits=[dict(kind='pipeline',event=event,values=dict(blend_state=dict(targets={str(i):dict(write_mask=0) for i in range(8)})))]
        muted=compare(label+'-write-disabled',path,edits,'hardware',event,False,ids)
        check(label+'-real-draw-color-suppression',original!=before and muted==before)
print(len(checks),'blend comparisons PASS',flush=True)
