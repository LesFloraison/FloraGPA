"""Compare native pipeline inspection with the preserved Python getters and scoped experiments."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

p=argparse.ArgumentParser()
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--exe',type=Path,required=True)
p.add_argument('--captures',type=Path,required=True)
p.add_argument('--fixtures',type=Path,required=True)
p.add_argument('--input-fixtures',type=Path,required=True)
p.add_argument('--qt-bin',type=Path)
p.add_argument('--isolated-env',action='store_true')
p.add_argument('--out',type=Path,required=True)
a=p.parse_args()
a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference.resolve()),str(a.reference.resolve().parent/'tools')]
from frame import Frame
from dx11 import Device
from engine import Engine
from experiments import Experiment
from replay_pipeline import inspect
from validate_rasterizer_extensions import fixture as raster_fixture

env=dict(os.environ)
if a.isolated_env:
    env={k:v for k,v in env.items() if k.upper() in {'SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}}
    env['PATH']=str(Path(os.environ['WINDIR'])/'System32')+os.pathsep+os.environ['WINDIR']
if a.qt_bin: env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH']
checks=[]

def check_case(key,path,event,before,driver='hardware',project=None):
    out=a.out/key
    cmd=[str(a.exe.resolve()),'replay-pipeline',str(path.resolve()),'--event',str(event),'--out',str(out.resolve())]
    if before: cmd+=['--before']
    if driver=='warp': cmd+=['--warp']
    if project: cmd+=['--experiment',str(project.resolve())]
    native=subprocess.run(cmd,env=env,capture_output=True,timeout=180)
    (a.out/(key+'.log')).write_bytes(native.stderr)
    with Frame(path) as frame:
        device=Device(driver)
        error=None
        try:
            try:
                engine=Engine(frame,device,experiment=Experiment(frame,project) if project else None)
                expected=inspect(engine,event,after=not before)
            except (ValueError,RuntimeError,KeyError) as caught: error=str(caught)
        finally: device.close()
    if error is not None:
        passed=native.returncode!=0
        result=dict(case=key,passed=passed,error=error)
    else:
        if native.returncode: raise RuntimeError(key+': '+native.stderr.decode(errors='replace'))
        actual=json.loads((out/'replay-pipeline.json').read_text())
        keys=('event','api','context','value_time','source','experiment_applied','fields','command_enabled','known_fields','unknown_fields','notes','limits')
        differences={k:dict(native=actual.get(k),python=expected[k]) for k in keys if actual.get(k)!=expected[k]}
        # State bookkeeping counts differ; actual draw/dispatch submission counts must agree.
        for name in set(actual['counts'])|set(expected['counts']):
            if name in {'Draw','DrawAuto','DrawIndexed','DrawInstanced','DrawIndexedInstanced',
                        'DrawInstancedIndirect','DrawIndexedInstancedIndirect','Dispatch','DispatchIndirect'} and actual['counts'].get(name,0)!=expected['counts'].get(name,0):
                differences[name]=dict(native=actual['counts'].get(name,0),python=expected['counts'].get(name,0))
        report=json.loads((out/'report.json').read_text())
        assert report['completed'] and report['reference_pixels_used'] is False
        assert not (out/'frame.rgba').exists(), 'Pipeline inspection must not perform output readback'
        assert not any(Path(x).name.lower().startswith(('python','gpa_','gpa-','tk8','tcl8')) for x in report['loaded_modules'])
        passed=not differences
        if differences: (a.out/(key+'-differences.json')).write_text(json.dumps(differences,indent=2))
        result=dict(case=key,passed=passed,fields=len(actual['fields']),known=actual['known_fields'])
    checks.append(result)
    (a.out/'validation.json').write_text(json.dumps(checks,indent=2)+'\n')
    assert passed,result
    print(key,'PASS',flush=True)

def project(path,capture,operations,cursor):
    path.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=hashlib.sha256(capture.read_bytes()).hexdigest(),
        cursor=cursor,history=[dict(label='Pipeline input test',operations=operations),
                              dict(label='Disable command',operations=[dict(kind='enabled',event=100,value=False)])])))
    return path

compute=a.fixtures/'compute.gpa_frame'
ops=[]
for resource,value in ((2,20),(4,70)):
    data=struct.pack('<I',value)
    ops.append(dict(kind='buffer',event=100,resource=resource,offset=0,
                    asset=dict(data=base64.b64encode(data).decode(),sha256=hashlib.sha256(data).hexdigest())))
for driver in ('hardware','warp'):
    for event in (95,96,97,100,105,110,112):
        for before in (True,False): check_case(f'compute-{driver}-{event}-{before}',compute,event,before,driver)
    for cursor,label in ((1,'clones'),(2,'disabled'),(0,'undo')):
        path=project(a.out/f'{driver}-{label}.json',compute,ops,cursor)
        for event in (100,105,110):
            for before in (True,False): check_case(f'{label}-{driver}-{event}-{before}',compute,event,before,driver,path)
    for name in ('inputs','gaps'):
        capture=a.input_fixtures/f'{name}-{driver}.gpa_frame'
        rows=json.loads(capture.with_suffix('.json').read_text())
        for row in rows:
            check_case(f'{name}-{driver}-{row["event"]}-{row["before"]}',capture,row['event'],row['before'],driver)
    for kind in (0x89,0x10e,0x10f):
        capture=a.out/f'raster-{driver}-{kind:x}.gpa_frame'
        raster_fixture(capture,kind=kind,forced=0 if kind==0x89 else 4,basic_blend=True)
        for before in (True,False): check_case(f'raster-{driver}-{kind:x}-{before}',capture,100,before,driver)

for file,events in [('GF2_Exilium_2026_03_03__00_19_35.gpa_frame',(79,430)),
                    ('bf1_2026_01_21__16_53_05.gpa_frame',(20434,25572))]:
    for event in events:
        for before in (True,False):
            check_case(f'{file[:3]}-{event}-{before}',a.captures/file,event,before)
print(len(checks),'checks PASS',flush=True)
