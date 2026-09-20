"""Development-only comparison of persistent shader setters with the preserved Python implementation."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--qt-bin', type=Path)
p.add_argument('--isolated-env', action='store_true')
p.add_argument('--out', type=Path, required=True)
p.add_argument('--oracle', type=Path, required=True)
p.add_argument('--real-only', action='store_true')
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference.resolve()), str(a.reference.resolve().parent / 'tools')]
from validate_class_linkage import fixture, clone, COMMON, POSITIONS
from validate_buffer_edits import experiment
from frame import Frame
from devices import create_device
from engine import Engine
from experiments import Experiment, blob
from class_linkage import describe
from shaders import compile_hlsl, resource_metadata
from validate_shader_setters import build
from setter_edits import captured, validate
import hashlib
from replay_pipeline import inspect

env = dict(os.environ)
if a.isolated_env:
    env = {k: v for k, v in env.items() if k.upper() in {'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA'}}
    env['PATH'] = str(Path(os.environ['WINDIR']) / 'System32') + os.pathsep + os.environ['WINDIR']
if a.qt_bin:
    env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + env['PATH']
checks = []


def check(name, passed, **detail):
    checks.append(dict(case=name, passed=bool(passed), **detail))
    (a.out / 'validation.json').write_text(json.dumps(checks, indent=2) + '\n')
    assert passed, checks[-1]
    print(name, 'PASS', flush=True)


def native(name, path, command, args=(), driver='hardware', project=None, failure=False):
    out = a.out / name
    cmd = [str(a.exe.resolve()), command, str(path.resolve()), '--out', str(out.resolve()), *args]
    if driver == 'warp':
        cmd += ['--warp']
    if project:
        cmd += ['--experiment', str(project.resolve())]
    result = subprocess.run(cmd, env=env, capture_output=True, timeout=90)
    (a.out / (name + '.log')).write_bytes(result.stderr)
    if failure:
        # An access violation is not an explicit rejection.
        assert result.returncode == 1, (name, result.returncode, result.stderr)
        assert b'"completed":false' in result.stderr, (name, result.stderr)
        return result.stderr.decode(errors='replace')
    assert result.returncode == 0, (name, result.stderr.decode(errors='replace'))
    report = json.loads((out / 'report.json').read_text())
    assert report['reference_pixels_used'] is False
    if command in {'replay', 'buffer', 'replay-pipeline'}:
        assert report['completed']
        assert not any(Path(m).name.lower().startswith(('python', 'gpa_', 'gpa-', 'tk8', 'tcl8')) for m in report['loaded_modules'])
    return out


def compare(name, path, stage, driver='hardware', project=None, event=100, pipeline=False, before=False):
    with Frame(path) as frame:
        device = create_device(driver)
        try:
            engine = Engine(frame, device, experiment=Experiment(frame, project) if project else None)
            if pipeline:
                expected = inspect(engine, event, after=not before)
            else:
                engine.replay(until=event, before=before, readback=False)
                expected = engine.read_buffer(74) if stage == 'cs' else engine.readback(30)[2]
        finally:
            device.close()
    args = ['--event', str(event)] + (['--before'] if before else [])
    if pipeline:
        out = native(name, path, 'replay-pipeline', args, driver, project)
        actual = json.loads((out / 'replay-pipeline.json').read_text())
        # Bookkeeping counts are implementation-specific; bindings and provenance are exact.
        keys = ('event', 'api', 'context', 'value_time', 'source', 'experiment_applied', 'fields', 'command_enabled', 'known_fields', 'unknown_fields', 'notes', 'limits')
        differences = {k: dict(native=actual.get(k), python=expected[k]) for k in keys if actual.get(k) != expected[k]}
        if differences:
            (a.out / (name + '-diff.json')).write_text(json.dumps(differences, indent=2))
        check(name, not differences, fields=len(actual['fields']))
    else:
        command = 'buffer' if stage == 'cs' else 'replay'
        out = native(name, path, command, args + ['--id', '74' if stage == 'cs' else '30'], driver, project)
        actual = (out / ('buffer.bin' if stage == 'cs' else 'frame.rgba')).read_bytes()
        check(name, actual == expected, bytes=len(actual))


if a.real_only:
    for filename,golden in {
        'GF2_Exilium_2026_03_03__00_19_35.gpa_frame':'2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1',
        'bf1_2026_01_21__16_53_05.gpa_frame':'1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6'}.items():
        path=a.reference.parent/filename
        with Frame(path) as frame:
            ops=[dict(kind='setter',event=e.id,values=dict(shader=0,class_instances=[])) for e in frame.entries.values() if e.category==7 and e.type==0x34de+9]
            project=a.out/(filename+'.json');experiment(frame,project,ops)
            device=create_device('hardware')
            try:
                engine=Engine(frame,device,experiment=Experiment(frame,project));expected=engine.replay()[2]
            finally:device.close()
        out=native(filename+'-edit',path,'replay',project=project)
        actual=(out/'frame.rgba').read_bytes()
        check(filename+' edited output',actual==expected and hashlib.sha256(actual).hexdigest()!=golden,setters=len(ops))
        doc=json.loads(project.read_text());doc['cursor']=0;project.write_text(json.dumps(doc))
        out=native(filename+'-undo',path,'replay',project=project)
        check(filename+' undo golden',hashlib.sha256((out/'frame.rgba').read_bytes()).hexdigest()==golden)
    sys.exit(0)

paths={};cases=[];expected=[]
def add(path,values=...,event=90):
    case=dict(path=str(path.resolve()),event=event)
    if values is not ...:case['values']=values
    cases.append(case)
    try:
        with Frame(path) as frame:
            en=frame.entries[event]
            value=captured(en.type,frame.payload(event)) if values is ... else validate(frame,en,values)
        expected.append(dict(status='ok',value=value))
    except (ValueError,KeyError,TypeError,OverflowError,struct.error):expected.append(dict(status='error'))
for stage in ('vs','hs','ds','gs','ps','cs'):
    for lifetime in ('persist','restore','clear'):
        paths[stage,lifetime]=build(a.out,stage,lifetime)
    path,info=paths[stage,'persist'];original=dict(shader=info['shader'],class_instances=[62])
    add(path);add(path,original)
    for values in (None,[],{},True,{**original,'extra':0},dict(shader=0,class_instances=[]),dict(shader=1002,class_instances=[1000])):add(path,values)
    for key in original:
        for value in (None,True,False,-1,2**64,'1',[],{},0,1,10,30,60,62,1002,2**64-1):add(path,{**original,key:value})
    for value in ([],[0],[1],[60],[1000],[62,1000],[62]*256,[62]*257,[True],[-1],['62'],[2**64]):add(path,{**original,'class_instances':value})
    with Frame(path) as frame:wire=frame.payload(90)
    for size in range(len(wire)):
        broken=clone(path,a.out/f'{stage}-wire-{size}.gpa_frame',lambda en,raw,size=size:raw[:size] if en.id==90 else raw)
        add(broken)
    for count,present,tail in ((0,0,b''),(0,1,b''),(1,0,wire[29:]),(257,1,wire[29:]*257),(1,2,wire[29:]),(1,1,wire[29:]+b'X')):
        raw=wire[:24]+struct.pack('<IB',count,present)+tail
        broken=clone(path,a.out/f'{stage}-layout-{count}-{present}-{len(tail)}.gpa_frame',lambda en,data,raw=raw:raw if en.id==90 else data)
        add(broken)
request=a.out/'request.json';request.write_text(json.dumps(dict(cases=cases)))
run=subprocess.run([str(a.oracle.resolve()),'--oracle',str(request.resolve())],env=env,capture_output=True,timeout=180)
assert run.returncode==0,run.stderr
actual=json.loads(run.stdout)
differences=[dict(case=case,expected=left,actual=right) for case,left,right in zip(cases,expected,actual,strict=True) if left['status']!=right['status'] or left['status']=='ok' and left['value']!=right.get('value')]
(a.out/'cpu-differences.json').write_text(json.dumps(differences,indent=2))
check('CPU shader wire and arguments',not differences,cases=len(cases),accepted=sum(x['status']=='ok' for x in expected))

for (stage,lifetime),(path,info) in paths.items():
    with Frame(path) as frame:
        projects={}
        for label,values in {'program':dict(shader=1002,class_instances=[62]),'class':dict(shader=info['shader'],class_instances=[1000]),'null':dict(shader=0,class_instances=[])}.items():
            project=a.out/f'{stage}-{lifetime}-{label}.json'
            experiment(frame,project,[dict(kind='setter',event=90,values=values)])
            projects[label]=project
    for driver in ('warp','hardware'):
        for label,project in projects.items():
            tag=f'{stage}-{lifetime}-{driver}-{label}'
            # Unbound graphics stages are inspected before submission; the Python baseline loses the NVIDIA device when drawing with a null VS.
            if label!='null' or stage in ('ps','cs'):
                for event in (100,200):compare(f'{tag}-{event}',path,stage,driver,project,event=event)
            compare(tag+'-setter',path,stage,driver,project,event=90,pipeline=True)
            compare(tag+'-snapshot',path,stage,driver,project,event=100,pipeline=True,before=True)
        if lifetime!='persist':
            compare(f'{stage}-{lifetime}-{driver}-reset',path,stage,driver,projects['program'],event=190,pipeline=True)

path,info=paths['ps','persist']
with Frame(path) as frame:
    missing=a.out/'missing.json'
    # Construct invalid projects directly; both loaders must reject before replay.
    valid=json.loads((a.out/'ps-persist-program.json').read_text())
    valid['history'][0]['operations'][0]['values']['class_instances']=[]
    missing.write_text(json.dumps(valid))
    try:Experiment(frame,missing);rejected=False
    except ValueError:rejected=True
    check('missing interfaces rejected',rejected and bool(native('missing',path,'replay',project=missing,failure=True)))
    asset=a.out/'static.dxbc';asset.write_bytes(compile_hlsl('float4 main():SV_Target{return float4(0,0,1,1);}','ps_5_0')[0])
    ops=[dict(kind='setter',event=90,values=dict(shader=1002,class_instances=[62])),dict(kind='shader',resource=1002,asset=blob(asset))]
    for reverse in (False,True):
        project=a.out/f'static-{reverse}.json';experiment(frame,project,list(reversed(ops)) if reverse else ops)
        for driver in ('warp','hardware'):
            compare(f'static-{reverse}-{driver}',path,'ps',driver,project)
            compare(f'static-{reverse}-{driver}-binding',path,'ps',driver,project,pipeline=True)
    # Ordered two-interface binding must change the actual output when reversed.
    multi=a.out/'two-interfaces.dxbc'
    multi.write_bytes(compile_hlsl(COMMON.replace('ITransform selected;', 'ITransform selected[2];') +
        'float4 main():SV_Target{return selected[0].apply(0)*.75+selected[1].apply(0)*.25;}', 'ps_5_0')[0])
    for reverse in (False,True):
        project=a.out/f'two-interfaces-{reverse}.json'
        experiment(frame,project,[dict(kind='shader',resource=1002,asset=blob(multi)),
            dict(kind='setter',event=90,values=dict(shader=1002,class_instances=[1000,62] if reverse else [62,1000]))])
        for driver in ('warp','hardware'):
            compare(f'two-interfaces-{reverse}-{driver}',path,'ps',driver,project)
            compare(f'two-interfaces-{reverse}-{driver}-binding',path,'ps',driver,project,pipeline=True)
    project=a.out/'ps-persist-program.json'
    from buffers import export_buffer
    for driver in ('warp','hardware'):
        device=create_device(driver)
        try:
            engine=Engine(frame,device,experiment=Experiment(frame,project))
            folder=a.out/f'constants-{driver}-python';folder.mkdir()
            report=export_buffer(engine,70,folder,event_id=100)
        finally:device.close()
        out=native(f'constants-{driver}',path,'buffer',['--id','70','--event','100','--before'],driver,project)
        actual=json.loads((out/'report.json').read_text())
        check(f'constants-{driver}',actual['constant_bindings']==report['constant_bindings'],shader=1002)
(a.out/'completed.json').write_text(json.dumps(dict(completed=True,checks=len(checks),executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest()),indent=2))
