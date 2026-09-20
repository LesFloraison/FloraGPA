"""Compare native experiment execution reports with Python history and replay boundaries."""
import argparse,base64,hashlib,json,os,struct,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--exe',type=Path,required=True)
p.add_argument('--qt-bin',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
from frame import Frame
from engine import Engine
from experiments import Experiment
from dx11 import Device
from shaders import compile_hlsl
from validate_buffer_edits import Fixture,compute_fixture
from validate_texture_outputs import fixture
env={k:v for k,v in os.environ.items() if k.upper() in ('SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA')}
env['PATH']=str(a.qt_bin.resolve())+os.pathsep+os.environ['WINDIR']+'/System32;'+os.environ['WINDIR']
checks=[]
def asset(data):return dict(data=base64.b64encode(data).decode(),sha256=hashlib.sha256(data).hexdigest())
def save(completed=False):
    (a.out/'validation.json').write_text(json.dumps(dict(completed=completed,passed=all(x['passed'] for x in checks),checks=checks,
        executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest()),indent=2))
def compare(name,path,project,driver,event,before=False,command='texture',suppress=False,rid=20):
    with Frame(path) as frame:
        device=Device(driver)
        try:
            exp=Experiment(frame,project) if project else None
            engine=Engine(frame,device,experiment=exp,suppress_draws=suppress)
            engine.replay(until=event,before=before,readback=False)
            expected=exp.report() if exp else None
        finally:device.close()
    out=a.out/(name+'-native')
    cmd=[str(a.exe.resolve()),command,str(path.resolve()),'--out',str(out.resolve())]
    if event is not None:cmd+=['--event',str(event)]
    if before:cmd+=['--before']
    if command in ('texture','buffer'):cmd+=['--id',str(rid)]
    if command=='texture':cmd+=['--no-preview']
    if driver=='warp':cmd+=['--warp']
    if project:cmd+=['--experiment',str(project.resolve())]
    if suppress:cmd+=['--suppress-draws']
    run=subprocess.run(cmd,env=env,capture_output=True,timeout=180)
    (a.out/(name+'.log')).write_bytes(run.stdout+run.stderr)
    differences={}
    if run.returncode:differences['native_error']=run.stderr.decode(errors='replace')
    else:
        got=json.loads((out/'report.json').read_text())
        # JSON canonicalization converts Python's integer dictionary keys to strings.
        expected=json.loads(json.dumps(expected))
        if got.get('experiment')!=expected:differences['report']=dict(expected=expected,actual=got.get('experiment'))
        if command=='texture' and event is not None:
            metadata=json.loads((out/'texture.json').read_text())
            if metadata.get('experiment')!=expected:differences['texture']=dict(expected=expected,actual=metadata.get('experiment'))
        modules=[Path(x).name.lower() for x in got['loaded_modules']]
        if any(x.startswith(('python','gpa-','gpa_','tk8','tcl8')) or x=='renderdoc.dll' for x in modules):differences['runtime']=modules
    checks.append(dict(name=name,passed=not differences,differences=differences));save()
    print(name,'PASS' if not differences else 'FAIL',flush=True)

for role in ('rtv','uav'):
    path=a.out/(role+'.gpa_frame');fmt=28 if role=='rtv' else 42
    layout=fixture(path,role=role,fmt=fmt)
    with Frame(path) as frame:
        f=Fixture();f.records=[]
        for e in frame.entries.values():f.add(e.id,e.category,e.type,frame.payload(e.id))
        sid=12 if role=='rtv' else 10;shader=frame.shader(frame.resource(sid)['data_id'])
    size=10*6*4;data=bytes((i*3+7)%256 for i in range(size))
    f.data(51,data)
    f.add(50,7,0x32 if role=='rtv' else 0x33,struct.pack('<QQQB',0,1,30,1)+struct.pack('<4f' if role=='rtv' else '<4I',0,0,0,1))
    f.add(60,7,0x247,struct.pack('<QQQIBQII',0,1,20,0,0,51,4096,8192))
    f.add(80,7,0x34ef,struct.pack('<QQQ',0,1,0))
    f.write(path)
    source='float4 main():SV_Target{return float4(0,1,0,1);}' if role=='rtv' else 'RWTexture2DArray<uint> dst:register(u0);[numthreads(1,1,1)]void main(){dst[uint3(0,0,0)]+=17;}'
    replacement=compile_hlsl(source,'ps_5_0' if role=='rtv' else 'cs_5_0')[0]
    sub=next(s for s in layout['subs'] if s['mip']==1 and s['layer']==1)
    history=[
        dict(label='Initial assets',operations=[dict(kind='shader',resource=sid,asset=asset(shader)),dict(kind='texture',resource=20,asset=asset(layout['initial'])),dict(kind='view',resource=30,values=dict(array_size=2))]),
        dict(label='Event edits',operations=[dict(kind='enabled',event=100,value=False),dict(kind='enabled',event=200,value=True),dict(kind='enabled',event=50,value=False),dict(kind='setter',event=80,values=dict(input_layout=0)),dict(kind='update_source',event=60,asset=asset(data))]),
        dict(label='Replace final assets',operations=[dict(kind='shader',resource=sid,asset=asset(replacement)),dict(kind='texture',resource=20,asset=asset(bytes([11])*len(layout['initial']))),dict(kind='view',resource=30,values=dict(array_size=1)),dict(kind='texture_output',event=200,resource=20,mip=1,layer=1,asset=asset(bytes([31])*sub['size']))]),
        dict(label='Enable and disable',operations=[dict(kind='enabled',event=100,value=True),dict(kind='enabled',event=200,value=False),dict(kind='update_source',event=60,asset=asset(bytes([47])*size))])]
    for cursor in range(5):
        project=a.out/(role+f'-{cursor}.json')
        project.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),cursor=cursor,history=history)))
        for driver in ('hardware','warp'):
            for event in (50,60,80,100,200):
                for before in (True,False):compare(f'{role}-{cursor}-{driver}-{event}-{before}',path,project,driver,event,before)
            compare(f'{role}-{cursor}-{driver}-full',path,project,driver,None,command='replay')
            compare(f'{role}-{cursor}-{driver}-suppress',path,project,driver,None,command='replay',suppress=True)
    compare(role+'-no-project',path,None,'warp',100)
# Preserve resource and event IDs beyond signed JSON integer range.
f=Fixture();rid=(1<<63)+20;event=(1<<63)+80
desc=[10,6,1,1,103,1,0,3,0,0x30000,0]
f.add(rid,5,0x85,struct.pack('<QQ11IQ',123,0,*desc,21))
f.data(21,bytes([123])*90);f.data(22,bytes((i*7+13)%256 for i in range(90)))
f.add(event,7,0x246,struct.pack('<QQiQIIIQ',0,1,0,rid,0,2,0,22))
path=a.out/'uint64.gpa_frame';f.write(path)
project=a.out/'uint64-project.json'
project.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),cursor=1,
    history=[dict(label='Large IDs',operations=[dict(kind='texture',resource=rid,asset=asset(bytes([31])*90)),dict(kind='enabled',event=event,value=True)])])))
for before in (True,False):compare('uint64-'+str(before),path,project,'warp',event,before,rid=rid)

# Buffer consumers share the report; global counters do not create event entries.
path=a.out/'counter.gpa_frame';compute_fixture(path,kind='counter')
# This older reference fixture leaves CopyStructureCount's context token zero.
# Give both implementations an explicit captured immediate context, as required
# by the native command validation; preserve the command and counter operations.
with Frame(path) as frame:
    f=Fixture();f.records=[]
    for entry in frame.entries.values():
        payload=bytearray(frame.payload(entry.id))
        if entry.type==0x3f:struct.pack_into('<Q',payload,8,1)
        f.add(entry.id,entry.category,entry.type,payload)
f.write(path)
project=a.out/'counter-project.json'
project.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),cursor=1,
    history=[dict(label='Buffer and counter edits',operations=[dict(kind='initial_uav_counter',view=46,value=2),
        dict(kind='uav_counter',event=100,view=46,value=1),dict(kind='buffer',event=100,resource=28,offset=0,asset=asset(struct.pack('<I',17))),
        dict(kind='enabled',event=200,value=False)])])))
for driver in ('hardware','warp'):
    for event in (100,200):
        for before in (True,False):compare(f'counter-{driver}-{event}-{before}',path,project,driver,event,before,command='buffer',rid=28)
save(True)
print('Checks',len(checks),'failures',sum(not x['passed'] for x in checks),flush=True)
raise SystemExit(0 if all(x['passed'] for x in checks) else 1)
