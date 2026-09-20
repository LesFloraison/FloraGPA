"""Real GF2 event input edit, native texture preview/storage and final replay parity."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--exe',type=Path,required=True)
p.add_argument('--qt-bin',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
sys.path.insert(0,str(a.reference/'standalone'))
from frame import Frame
from dx11 import Device
from engine import Engine
from experiments import Experiment
from events import draw_event
from presentation import select

path=a.reference/'GF2_Exilium_2026_03_03__00_19_35.gpa_frame'
data=bytes(256*256)
op=dict(kind='texture_input',event=1455,resource=1066,mip=0,layer=0,
        asset=dict(data=base64.b64encode(data).decode(),sha256=hashlib.sha256(data).hexdigest()))
document=dict(format='FloraGPA experiment 1',frame_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
              cursor=1,history=[dict(label='Input texture edit',operations=[op])])
project=a.out/'input.json';project.write_text(json.dumps(document))
env=os.environ.copy();env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH']
checks=[]
def check(label,passed,**details):
    checks.append(dict(name=label,passed=bool(passed),**details))
    (a.out/'validation.json').write_text(json.dumps(dict(completed=False,passed=all(x['passed'] for x in checks),checks=checks),indent=2))
    print(label,'PASS' if passed else 'FAIL',flush=True)
    assert passed,(label,details)
def native(label,command,extra):
    out=a.out/label
    run=subprocess.run([str(a.exe.resolve()),command,str(path.resolve()),'--experiment',str(project.resolve()),'--out',str(out.resolve()),*extra],env=env,capture_output=True,timeout=180)
    (a.out/(label+'.log')).write_bytes(run.stdout+run.stderr);run.check_returncode()
    report=json.loads((out/'report.json').read_text())
    modules=[Path(x).name.lower() for x in report['loaded_modules']]
    check(label+'-native-runtime',not any(x.startswith(('python','gpa-','gpa_','tk8','tcl8')) or x=='renderdoc.dll' for x in modules))
    return out
for before in (True,False):
    with Frame(path) as frame:
        d=Device()
        try:
            exp=Experiment(frame,project);engine=Engine(frame,d,experiment=exp)
            engine.replay(until=1455,before=before,readback=False)
            if before:
                with exp.inputs(engine,draw_event(frame,frame.entries[1455])): expected=engine.read_texture(1066)
            else:expected=engine.read_texture(1066)
        finally:d.close()
    extra=['--id','1066','--event','1455']+(['--before'] if before else [])
    out=native('before' if before else 'after','texture-storage',extra)
    check('before-input' if before else 'after-original',(out/'texture.bin').read_bytes()==expected)
    if before:check('before-is-imported-raw',expected==data)
    else:check('after-restores-different-original',expected!=data)
    out=native('preview-before' if before else 'preview-after','texture',extra)
    if before:check('preview-before-is-zero',not any((out/'frame.rgba').read_bytes()))
for undo in (False,True):
    document['cursor']=0 if undo else 1;project.write_text(json.dumps(document))
    with Frame(path) as frame:
        d=Device()
        try:
            engine=Engine(frame,d,experiment=Experiment(frame,project));engine.replay(readback=False)
            selected=select(engine,None); _,_,expected=engine.readback(selected['resource'],view=selected['view'])
        finally:d.close()
    out=native('undo' if undo else 'edited','replay',[])
    actual=(out/'frame.rgba').read_bytes()
    check('undo-parity' if undo else 'edited-parity',actual==expected)
    golden='2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1'
    digest=hashlib.sha256(actual).hexdigest()
    check('undo-golden' if undo else 'edited-changes-frame',(digest==golden) if undo else (digest!=golden),sha256=digest)
(a.out/'validation.json').write_text(json.dumps(dict(completed=True,passed=True,checks=checks,
    executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest()),indent=2))
