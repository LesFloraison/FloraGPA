"""Development-only native SO/DrawAuto comparisons against the preserved Python engine."""
import argparse
import hashlib
import json
import math
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
p.add_argument('--captured-dir', type=Path, help='Optional preserved original GPA SO capture suite')
p.add_argument('--linear-captured-dir', type=Path, help='Optional preserved original GPA point/line SO suite')
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference.resolve()), str(a.reference.resolve().parent / 'tools')]
from frame import Frame
from devices import create_device
from engine import Engine
from experiments import Experiment, blob
from validate_stream_output import fixture, PRODUCER
from validate_so_history import fixture as history_fixture, wire
from validate_so_passthrough import fixture as passthrough_fixture, multi_fixture
from validate_buffer_edits import Fixture, experiment, operation
from stream_output import declaration
from shaders import compile_hlsl
from geometry import export_geometry
from replay_pipeline import inspect as pipeline

env = dict(os.environ)
if a.isolated_env:
    env = {k:v for k,v in env.items() if k.upper() in {'SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}}
    env['PATH'] = os.environ['WINDIR'] + '/System32;' + os.environ['WINDIR']
if a.qt_bin:
    env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + env['PATH']
checks = []
def check(name, passed, **detail):
    checks.append(dict(case=name, passed=bool(passed), **detail))
    (a.out/'validation.json').write_text(json.dumps(checks, indent=2)+'\n')
    assert passed, checks[-1]
    print(name, 'PASS', flush=True)

def native(name, path, command, driver, event, args=(), project=None, fail=False):
    out = a.out / name
    cmd = [str(a.exe.resolve()),command,str(path.resolve()),'--out',str(out.resolve()),*args]
    if event: cmd += ['--event',str(event)]
    if driver == 'warp': cmd += ['--warp']
    if project: cmd += ['--experiment',str(project.resolve())]
    result = subprocess.run(cmd,env=env,capture_output=True,timeout=90)
    (a.out/(name+'.log')).write_bytes(result.stderr)
    if fail:
        assert result.returncode == 1 and b'"completed":false' in result.stderr,(name,result.returncode,result.stderr)
        return
    assert result.returncode == 0,(name,result.returncode,result.stderr.decode(errors='replace'))
    report = json.loads((out/'report.json').read_text())
    assert report['reference_pixels_used'] is False
    if command != 'shader':
        assert report['completed']
        assert not any(Path(m).name.lower().startswith(('python','gpa_','gpa-','tk8','tcl8')) or Path(m).name.lower()=='renderdoc.dll' for m in report['loaded_modules'])
    return out, report

def clone(path, output, change=lambda e,r:r, extra=lambda x:None):
    x=Fixture(); x.records=[]
    with Frame(path) as f:
        for en in f.entries.values():
            raw=change(en,f.payload(en.id))
            if raw is not None: x.add(en.id,en.category,en.type,raw)
    extra(x);x.write(output);return output

def compare(name,path,driver='hardware',event=200,project=None,geometry=False,before=False):
    name += '-'+driver+'-'+str(event)+('-before' if before else '')
    with Frame(path) as f:
        d=create_device(driver)
        try:
            e=Engine(f,d,experiment=Experiment(f,project) if project else None)
            e.replay(until=event,before=before,readback=False)
            targets=[rid for rid in (40,42,44,46) if rid in f.entries]
            expected={rid:e.read_buffer(rid) for rid in targets}
            pixels=e.read_texture(50)
            history=e.so_count_history
            automatic=e.so_auto_results.get(event)
            decl=declaration(f,14)
            if geometry: expected_geometry=export_geometry(e,event,a.out/(name+'-python-geometry'))
        finally: d.close()
    for rid,raw in expected.items():
        output,report=native(name+'-buffer-'+str(rid),path,'buffer',driver,event,['--id',str(rid)]+(['--before'] if before else []),project)
        check(name+'-buffer-'+str(rid),(output/'buffer.bin').read_bytes()==raw,bytes=len(raw))
    output,report=native(name+'-image',path,'replay',driver,event,['--id','50']+(['--before'] if before else []),project)
    check(name+'-image',(output/'frame.rgba').read_bytes()==pixels,bytes=len(pixels))
    actual=[{k:int(v) for k,v in row.items()} for row in report['stream_output_history']]
    check(name+'-history',actual==history,queries=len(actual))
    if automatic is not None:
        check(name+'-auto',report['draw_auto']==automatic,vertices=automatic['parameters']['vertex_count'])
    if geometry:
        output,report=native(name+'-geometry',path,'geometry',driver,event,project=project)
        actual=json.loads((output/'geometry.json').read_text())
        keys=('effective_parameters','draw_auto','vertex_references','unique_vertices','strip_restart_references','obj_vertices','obj_faces','obj_lines','obj_points')
        differences={k:(actual.get(k),expected_geometry[k]) for k in keys if actual.get(k)!=expected_geometry[k]}
        for file in ('vertices.csv','unique_vertices.csv','references.csv'):
            # CSV quoting/float spelling may vary; compare parsed cells numerically where applicable.
            import csv
            left=list(csv.reader((output/file).open(newline='')))
            right=list(csv.reader((a.out/(name+'-python-geometry')/file).open(newline='')))
            def normalize(rows):
                def cell(x):
                    try:return float(x)
                    except ValueError:return x
                return [[cell(x) for x in row] for row in rows]
            if normalize(left)!=normalize(right):differences[file]=(left,right)
        if differences:(a.out/(name+'-geometry-diff.json')).write_text(json.dumps(differences,indent=2))
        check(name+'-geometry',not differences)

base=a.out/'interleaved.gpa_frame';fixture(base)
for name,options in [('interleaved',{}),('paired',dict(paired=True)),('gap',dict(gap=True)),('append',dict(append=True)),('implicit',dict(implicit_stride=True)),('full',dict(full_targets=True)),('null-gs',dict(auto_gs=0)),('captured-zero',dict(auto_count=0))]:
    path=base if name=='interleaved' else a.out/(name+'.gpa_frame')
    if path!=base:fixture(path,**options)
    for driver in ('hardware','warp'):
        for event in (100,150,200):compare(name,path,driver,event,geometry=event==200)
    out,report=native(name+'-shader',path,'shader','hardware',None,['--id','12'])
    with Frame(path) as f:
        info=json.loads((out/'shader.json').read_text())
        check(name+'-declaration',info['stream_output']==declaration(f,14) and (out/'shader.dxbc').read_bytes()==f.shader(13))

for name,commands,offsets,dirty in [
    ('unchanged',[],(16,),False),('dirty',[],(16,),True),
    ('same-reset',[(120,(40,),(16,))],(16,),False),
    ('reset-append',[(120,(40,),(64,)),(125,(),()),(130,(40,),(0xffffffff,))],(0xffffffff,),False),
    ('null-offsets',[(120,(40,),None)],(16,),False),
    ('unbind-append',[(120,(),()),(130,(40,),(0xffffffff,))],(0xffffffff,),False),
    ('clear-reset',[(120,'clear',None),(130,(40,),(64,))],(64,),False)]:
    path=history_fixture(a.out/(name+'.gpa_frame'),commands,offsets,dirty)
    for driver in ('hardware','warp'):compare(name,path,driver,geometry=True)
for version in range(5):
    path=history_fixture(a.out/f'context-{version}.gpa_frame',[(120,(40,),(64,))],(64,),version=version)
    compare('context-'+str(version),path)

for name,options in [('vs',{}),('vs4',dict(profile='vs_4_0')),('signature',dict(signature_only=True)),('ds',dict(profile='ds_5_0',tessellated=True)),('ds-signature',dict(profile='ds_5_0',signature_only=True,tessellated=True))]:
    path=passthrough_fixture(a.out/(name+'.gpa_frame'),**options)
    for driver in ('hardware','warp'):compare(name,path,driver,geometry=True)
    out,_=native(name+'-shader',path,'shader','hardware',None,['--id','12'])
    info=json.loads((out/'shader.json').read_text())
    check(name+'-passthrough',info['passthrough'] and info['pipeline_stage']=='gs' and info['interface_slots']==0)
for stream in (0,1,2,3,0xffffffff):
    path=a.out/f'multi-{stream}.gpa_frame';multi_fixture(path,stream)
    for driver in ('hardware','warp'):compare('multi-'+str(stream),path,driver)

history=history_fixture(a.out/'edit-base.gpa_frame')

# A three-vertex line strip writes two expanded lines (four SO vertices),
# while point output writes three primitives. Exercise each query conversion.
for name,stream_type,topology in [('lines','LineStream',2),('points','PointStream',1)]:
    code=compile_hlsl(PRODUCER.replace('TriangleStream',stream_type),'gs_5_0')[0]
    def topology_change(en,raw):
        if en.id==13:return struct.pack('<Q',len(code))+code+bytes(8)
        if en.id==199:
            raw=bytearray(raw)
            struct.pack_into('<Q',raw,10880,0)
            struct.pack_into('<I',raw,152,topology)
        return raw
    path=clone(history,a.out/(name+'.gpa_frame'),topology_change)
    for driver in ('hardware','warp'):compare(name,path,driver,geometry=True)

# Linked GS keeps the SO declaration and ordered class interfaces together.
linked_source = '''interface I{float4 color();};class A:I{float4 value;float4 color(){return value;}};
class B:I{float4 value;float4 color(){return value*.5;}};cbuffer Classes:register(b0){A first;B second;}I selected;
''' + PRODUCER.replace('o.color=float4(1,0,0,1);','o.color=selected.color();')
linked_code=compile_hlsl(linked_source,'gs_5_0')[0]
def linked_change(en,raw):
    raw=bytearray(raw)
    if en.id==12:struct.pack_into('<Q',raw,32,1000)
    if en.id==13:raw=struct.pack('<Q',len(linked_code))+linked_code+bytes(8)
    if en.id in (99,149):
        struct.pack_into('<Q',raw,10640,1002)
        struct.pack_into('<Q',raw,11912,1004)
        struct.pack_into('<I',raw,13960,1)
    return raw
def linked_extra(x):
    x.add(1000,5,0x97,bytes(16))
    x.buffer(1002,[32,0,4,0,0,0],struct.pack('<8f',1,0,0,1,0,1,0,1))
    x.add(1004,5,0x98,struct.pack('<QQ8IQ',0,1000,0,0,0,0,0,0,0,0,1006))
    x.add(1006,9,0x89,struct.pack('<I',6)+b'first\0'+bytes(4))
linked=clone(history,a.out/'linked-gs.gpa_frame',linked_change,linked_extra)
for driver in ('hardware','warp'):compare('linked-gs',linked,driver,geometry=True)

for name,code,operations in [
    ('disabled',None,[dict(kind='enabled',event=150,value=False)]),
    ('doubled',PRODUCER.replace('[maxvertexcount(3)]','[maxvertexcount(6)]').replace('V o;','for(uint i=0;i<2;i++){V o;')+'}',[]),
    ('overflow',PRODUCER.replace('[maxvertexcount(3)]','[maxvertexcount(30)]').replace('V o;','for(uint i=0;i<10;i++){V o;')+'}',[]),
    ('zero',PRODUCER.replace('V o;','if(input[0].x<100)return;V o;'),[]),
    ('input-clone',None,[operation(40,32,struct.pack('<4f',0,1,0,1),200)]),
    ('output-patch',None,[operation(40,0,b'ABCD',100)])]:
    project=a.out/(name+'.json')
    if code:
        file=a.out/(name+'.dxbc');file.write_bytes(compile_hlsl(code,'gs_5_0')[0])
        operations=operations+[dict(kind='shader',resource=12,asset=blob(file))]
    with Frame(history) as f:experiment(f,project,operations)
    for driver in ('hardware','warp'):compare(name,history,driver,project=project,geometry=True)

# Pre-frame append has no reconstructible cursor; preserve the captured count only without edits.
def unknown(en,raw):
    if en.id in (100,150):return None
    return raw
path=clone(base,a.out/'unknown.gpa_frame',unknown)
for driver in ('hardware','warp'):compare('unknown',path,driver,geometry=True)
with Frame(path) as f:experiment(f,a.out/'unknown-edited.json',[dict(kind='enabled',event=200,value=True)])
for driver in ('hardware','warp'):
    native('unknown-edited-'+driver,path,'replay',driver,200,project=a.out/'unknown-edited.json',fail=True)
    check('unknown-edited-'+driver,True)

# Selected setter boundaries report real native bindings; offsets remain unknown to getters.
path=history_fixture(a.out/'setter-boundaries.gpa_frame',[(120,(40,),(64,)),(125,(),()),(130,(40,),None)])
for event in (120,125,130):
    for before in (True,False):
        name=f'pipeline-{event}-{before}'
        with Frame(path) as f:
            d=create_device('warp')
            try:expected=pipeline(Engine(f,d),event,after=not before)
            finally:d.close()
        out,_=native(name,path,'replay-pipeline','warp',event,['--before'] if before else [])
        actual=json.loads((out/'replay-pipeline.json').read_text())
        check(name,actual['fields']==expected['fields'] and actual['limits']==expected['limits'],fields=len(actual['fields']))

for name,wire_bytes in [('missing-array',wire(None,None,count=1)),('bad-offset',wire((40,),(1,))),
                       ('bad-buffer',wire((50,),(0,))),('too-many',struct.pack('<QQIBB',0,1,5,0,0))]:
    path=clone(base,a.out/(name+'.gpa_frame'),extra=lambda x:x.add(120,7,0x3503,wire_bytes))
    with Frame(path) as f:
        d=create_device('warp');rejected=False
        try:
            try:Engine(f,d).replay(until=120,readback=False)
            except (ValueError,RuntimeError):rejected=True
        finally:d.close()
    native(name,path,'replay','warp',120,fail=True)
    check(name,rejected)
if a.captured_dir or a.linear_captured_dir:
    captured=[]
    if a.captured_dir:
        manifest=json.loads((a.captured_dir/'manifest.json').read_text())
        captured.extend((case,a.captured_dir/case['file']) for case in manifest['cases'])
    if a.linear_captured_dir:
        manifest=json.loads((a.linear_captured_dir/'manifest.json').read_text())
        for case in manifest['cases']:
            if not case['name'].startswith('so-'):continue
            case['draws']=[dict(id=eid) for eid in case['draws']]
            captured.append((case,a.linear_captured_dir/case['file']))
    for case,path in captured:
        check('original-'+case['name']+'-hash',hashlib.sha256(path.read_bytes()).hexdigest()==case['sha256'])
        aid=case['draws'][-1]['id']
        buffers=sorted({rid for state in case['states'] for rid in state['so'] if rid})
        edits=[('original',None)]
        with Frame(path) as f:
            for draw in case['draws'][:-1]:
                project=a.out/f"original-{case['name']}-disable-{draw['id']}.json"
                experiment(f,project,[dict(kind='enabled',event=draw['id'],value=False)])
                edits.append(('disable-'+str(draw['id']),project))
        for driver in ('hardware','warp'):
            for label,project in edits:
                name=f"original-{case['name']}-{driver}-{label}"
                with Frame(path) as f:
                    d=create_device(driver)
                    try:
                        e=Engine(f,d,experiment=Experiment(f,project) if project else None)
                        w,h,pixels=e.replay()
                        expected={rid:e.read_buffer(rid) for rid in buffers}
                        automatic=e.so_auto_results[aid]
                        history=e.so_count_history
                        inspections=e.counts.get('inspection_records',0)
                    finally:d.close()
                out,report=native(name,path,'replay',driver,aid,project=project)
                raw=(out/'frame.rgba').read_bytes()
                check(name+'-image',raw==pixels and (label!='original' or hashlib.sha256(raw).hexdigest()==case['rgba_sha256']),width=w,height=h)
                check(name+'-auto',report['draw_auto']==automatic)
                actual=[{k:int(v) for k,v in row.items()} for row in report['stream_output_history']]
                check(name+'-history',actual==history)
                for rid,raw in expected.items():
                    out,_=native(name+'-buffer-'+str(rid),path,'buffer',driver,aid,['--id',str(rid)],project)
                    check(name+'-buffer-'+str(rid),(out/'buffer.bin').read_bytes()==raw and (label!='original' or hashlib.sha256(raw).hexdigest()==case['so_sha256']))
                # Read the entire capture, including lifetime/getter calls after the last draw.
                # This is the same traversal used when opening the capture and collecting in Qt.
                out,report=native(name+'-full',path,'replay',driver,None,['--timings'],project)
                timings=report['timings']
                check(name+'-full', (out/'frame.rgba').read_bytes()==pixels
                      and report['counts'].get('inspection_records',0)==inspections
                      and len(timings)==len(case['draws'])-(label!='original')
                      and all(math.isfinite(row['microseconds']) and row['microseconds']>=0 for row in timings),
                      inspections=inspections,timestamps=len(timings))
print(len(checks),'stream-output checks PASS',flush=True)
