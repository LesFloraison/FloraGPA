"""Compare native event texture edits against Python on actual draw/dispatch fixtures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--oracle',type=Path,required=True)
p.add_argument('--qt-bin',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args(); a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
from frame import Frame
from dx11 import Device
from engine import Engine
from experiments import Experiment
from events import draw_event
from msaa import inspect_msaa
from formats import texture_info,subresources
from validate_texture_outputs import fixture,patch
from validate_msaa_edits import fixture as msaa_fixture,sample_patch

cases=[]; expected=[]
def project(path,ops,label):
    result=a.out/(label+'.json')
    result.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                                     cursor=len(ops),history=[dict(label=label,operations=[op]) for op in ops])))
    return result
def add(label,path,project_path,warp=False,event=100,before=False,resource=20,sample=None,fmt=None):
    item=dict(label=label,path=str(path.resolve()),event=event,before=before,resource=resource,warp=warp)
    if project_path:item['project']=str(project_path.resolve())
    if sample is not None:item.update(sample=sample,format=fmt)
    with Frame(path) as f:
        d=Device('warp' if warp else 'hardware')
        try:
            exp=Experiment(f,project_path) if project_path else None
            engine=Engine(f,d,experiment=exp);engine.replay(until=event,before=before,readback=False)
            def read():
                return inspect_msaa(d,engine.resources.get(resource),f.resource(resource),sample,fmt)[1] if sample is not None else engine.read_texture(resource)
            if before and exp:
                with exp.inputs(engine,draw_event(f,f.entries[event])):data=read()
            else:data=read()
            expected.append(dict(status='ok',bytes=data.hex()))
        finally:d.close()
    cases.append(item)
    print('Reference',label,flush=True)

for warp in (False,True):
    for dimension in ('1d','2d','3d','cube'):
        for role in ('rtv','uav','om_uav'):
            label=f'{warp}-{dimension}-{role}'
            path=a.out/(label+'.gpa_frame'); meta=fixture(path,dimension,role,42,partial_volume=dimension=='3d')
            sub=next(s for s in meta['subs'] if s['mip']==1 and s['layer']==meta['first'])
            data=struct.pack('<I',21)*(sub['size']//4)
            proj=project(path,[patch(20,1,meta['first'],data)],label)
            add(label+'-before',path,proj,warp,before=True)
            add(label+'-after',path,proj,warp)
            add(label+'-later',path,proj,warp,event=200)
    for fmt in (20,40,45,55):
        for readonly in (False,True):
            label=f'{warp}-depth-{fmt}-{readonly}';path=a.out/(label+'.gpa_frame')
            meta=fixture(path,'2d','dsv',fmt,readonly=readonly)
            sub=next(s for s in meta['subs'] if s['mip']==1 and s['layer']==1)
            pixel={20:struct.pack('<fI',.5,0x5a),40:struct.pack('<f',.5),45:struct.pack('<I',0x5a7fffff),55:struct.pack('<H',0x7fff)}[fmt]
            proj=project(path,[patch(20,1,1,pixel*(sub['size']//len(pixel)))],label)
            add(label+'-before',path,proj,warp,before=True);add(label+'-after',path,proj,warp)
    label=f'{warp}-alias';path=a.out/(label+'.gpa_frame');meta=fixture(path,'2d','rtv',42,alias=True)
    ops=[]
    for mip,kind,value in ((1,'texture_output',37),(0,'texture_input',51)):
        sub=next(s for s in meta['subs'] if s['mip']==mip and s['layer']==1)
        ops.append(patch(20,mip,1,struct.pack('<I',value)*(sub['size']//4),kind=kind))
    for reverse in (False,True):
        proj=project(path,list(reversed(ops)) if reverse else ops,label+str(reverse))
        add(label+f'-{reverse}-before',path,proj,warp,before=True);add(label+f'-{reverse}-after',path,proj,warp)
    for depth in (False,True):
        for input_edit in (False,True):
            label=f'{warp}-msaa-{depth}-{input_edit}';path=a.out/(label+'.gpa_frame');msaa_fixture(path,input_edit,depth)
            pixel=struct.pack('<H',0x4567) if depth else bytes([71,83,127,255])
            proj=project(path,[sample_patch(20,1,2,pixel*35,kind='texture_input' if input_edit else 'texture_output')],label)
            for before in (False,True):
                for sample in range(4):add(label+f'-{before}-{sample}',path,proj,warp,before=before,sample=sample,fmt=55 if depth else 28)
            if input_edit:add(label+'-result',path,proj,warp,resource=40)

request=a.out/'request.json';request.write_text(json.dumps(dict(cases=cases)))
env=os.environ.copy();env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH']
run=subprocess.run([str(a.oracle.resolve()),'--oracle',str(request.resolve())],env=env,capture_output=True,timeout=600)
(a.out/'native.log').write_bytes(run.stderr);run.check_returncode();actual=json.loads(run.stdout)
diffs=[]
for item,want,got in zip(cases,expected,actual):
    if want!=got:
        diffs.append(dict(case=item,expected=want,actual=got))
report=dict(completed=True,passed=len(actual)==len(expected) and not diffs,count=len(cases),differences=diffs,
            executable_sha256=hashlib.sha256(a.oracle.read_bytes()).hexdigest())
(a.out/'validation.json').write_text(json.dumps(report,indent=2))
print(json.dumps({k:v for k,v in report.items() if k!='differences'},indent=2))
if diffs:print(json.dumps(diffs[:1],indent=2))
raise SystemExit(0 if report['passed'] else 1)
