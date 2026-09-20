"""Compare native planar writes with Python using original captured Map/Update calls."""
import argparse, base64, hashlib, json, os, struct, subprocess, sys
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
from formats import texture_info
from update_sources import describe
from validate_legacy_planar_maps import initial_overrides
from validate_buffer_edits import Fixture

env={k:v for k,v in os.environ.items() if k.upper() in ('SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA')}
env['PATH']=str(a.qt_bin.resolve())+os.pathsep+os.environ['WINDIR']+'/System32;'+os.environ['WINDIR']
checks=[]
def save(completed=False):
    (a.out/'validation.json').write_text(json.dumps(dict(completed=completed,passed=all(x['passed'] for x in checks),
        checks=checks,executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest()),indent=2))
def asset(data):return dict(data=base64.b64encode(data).decode(),sha256=hashlib.sha256(data).hexdigest())
def compare(name,path,driver,event=4,rid=5,layer=1,before=False,overrides=None,update=None,full=False,discard=False):
    project=None
    ops=[dict(kind='texture',resource=key,asset=asset(data)) for key,data in (overrides or {}).items()]
    if update is not None:ops.append(dict(kind='update_source',event=event,asset=asset(update)))
    if ops:
        project=a.out/(name+'.json')
        project.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            cursor=1,history=[dict(label='Explicit planar assets',operations=ops)])))
    expected=None;records=[];error=None;info=None
    with Frame(path) as frame:
        device=Device(driver)
        try:
            engine=Engine(frame,device,experiment=Experiment(frame,project) if project else None)
            try:
                if full:
                    width,height,expected=engine.replay()
                    if discard:expected=expected[0::4]
                else:
                    engine.replay(until=event,before=before,readback=False)
                    expected=engine.read_texture(rid)
                    info=texture_info(frame.resource(rid))
                    if discard and not before:
                        size=info['width']*info['height']*(1 if info['format']==103 else 2)
                        offset=layer*size*3//2;expected=expected[offset:offset+size]
                records=engine.planar_writes
            except (ValueError,RuntimeError,KeyError) as e:error=str(e)
        finally:device.close()
    out=a.out/(name+'-native')
    cmd=[str(a.exe.resolve()),'replay' if full else 'texture',str(path.resolve()),'--out',str(out.resolve())]
    if not full:cmd+=['--id',str(rid),'--event',str(event),'--layer',str(layer),'--plane','y','--no-preview']
    if driver=='warp':cmd+=['--warp']
    if before:cmd+=['--before']
    if project:cmd+=['--experiment',str(project.resolve())]
    run=subprocess.run(cmd,env=env,capture_output=True,timeout=180)
    (a.out/(name+'.log')).write_bytes(run.stdout+run.stderr)
    differences={}
    if error:
        diagnostics=[]
        for line in run.stderr.decode(errors='replace').splitlines():
            try:diagnostics.append(json.loads(line))
            except ValueError:pass
        if run.returncode!=1 or not any(isinstance(x,dict) and x.get('completed') is False and x.get('error') for x in diagnostics):
            differences['rejection']=dict(code=run.returncode,stderr=run.stderr.decode(errors='replace'))
    elif run.returncode:
        differences['native_error']=run.stderr.decode(errors='replace')
    else:
        report=json.loads((out/'report.json').read_text())
        value=(out/('frame.rgba' if full else 'texture.bin')).read_bytes()
        if discard and not before:value=value[0::4] if full else value[offset:offset+size]
        if expected!=value:differences['storage']=dict(expected=hashlib.sha256(expected).hexdigest(),actual=hashlib.sha256(value).hexdigest())
        if records!=report['planar_writes']:differences['provenance']=dict(expected=records,actual=report['planar_writes'])
        if not full:
            metadata=json.loads((out/'texture.json').read_text())
            if records!=metadata['planar_writes']:differences['texture_provenance']=metadata['planar_writes']
        modules=[Path(x).name.lower() for x in report['loaded_modules']]
        if any(x.startswith(('python','gpa-','gpa_','tk8','tcl8')) or x=='renderdoc.dll' for x in modules):differences['runtime']=modules
    checks.append(dict(name=name,passed=not differences,expected_error=error,native_returncode=run.returncode,differences=differences));save()
    print(name,'PASS' if not differences else 'FAIL',flush=True)

root=a.reference/'output/planar-writes/captured-3'
for case in json.loads((root/'manifest.json').read_text())['cases']:
    path=root/case['label']/'capture.gpa_frame'
    write=case['writes'][0];fields=write['fields'];layer=fields['subresource'];discard=fields.get('map_type')==4
    for driver in ('hardware','warp'):
        prefix=case['label']+'-'+driver
        for before in (True,False):compare(prefix+('-before' if before else '-after'),path,driver,layer=layer,before=before,discard=discard)
        with Frame(path) as frame:
            if case['format']==104:overrides=initial_overrides(frame)
            else:
                r=frame.resource(5);raw=bytearray(frame.data(r['data_id']))
                for index in range(r['desc'][3]):raw[index*90+60:index*90+90]=bytes([123])*30
                overrides={5:bytes(raw)}
            update=None
            if write['name']=='UpdateSubresource':
                layout=describe(frame,4);update=bytes((i*7+31)%256 for i in range(layout['size']))
        for before in (True,False):compare(prefix+('-asset-before' if before else '-asset-after'),path,driver,layer=layer,before=before,overrides=overrides,update=update,discard=discard)
        compare(prefix+'-full',path,driver,layer=layer,overrides=overrides,update=update,discard=discard,full=True)

for name in ('map-write','map-read-write','map-discard'):
    path=a.reference/'output/planar-maps/p016-1'/name/'capture.gpa_frame'
    with Frame(path) as frame:overrides=initial_overrides(frame)
    for driver in ('hardware','warp'):
        for before in (True,False):compare('105-'+name+'-'+driver+('-before' if before else '-after'),path,driver,layer=0 if name=='map-discard' else 1,before=before,overrides=overrides,discard=name=='map-discard')
        compare('105-'+name+'-'+driver+'-full',path,driver,overrides=overrides,discard=name=='map-discard',full=True)

# Reject malformed writes before native resource creation or command submission.
for fmt in (103,104,105):
    unit=1 if fmt==103 else 2
    for name,kind,index,box,length,typ,map_type in (
        ('map-short','map',0,None,90*unit-1,1,2),('map-long','map',0,None,90*unit+1,1,2),
        ('map-subresource','map',2,None,90*unit,1,2),('map-no-overwrite','map',0,None,90*unit,1,5),
        ('map-diff','map',0,None,0,0x100,2),('update-short','update',0,None,90*unit-1,1,2),
        ('update-subresource','update',2,None,90*unit,1,2),('odd-box','update',0,[1,0,0,9,6,1],72*unit,1,2),
        ('outside-box','update',0,[0,0,0,12,6,1],108*unit,1,2),('depth-box','update',0,[0,0,1,10,6,2],90*unit,1,2)):
        f=Fixture();desc=[10,6,1,2,fmt,1,0,3 if kind=='map' else 0,0 if kind=='map' else 8,0x30000 if kind=='map' else 0,0]
        f.add(20,5,0x85,struct.pack('<QQ11IQ',123,0,*desc,0))
        raw=bytes(length);f.add(21,9,typ,struct.pack('<I',len(raw))+raw if typ==1 else struct.pack('<II',0,0))
        payload=struct.pack('<QQiQIIIQ',0,1,0,20,index,map_type,0,21) if kind=='map' else struct.pack('<QQQIB',0,1,20,index,bool(box))+(struct.pack('<6I',*box) if box else b'')+struct.pack('<QII',21,10*unit,90*unit)
        f.add(80,7,0x246 if kind=='map' else 0x247,payload)
        path=a.out/(str(fmt)+'-'+name+'.gpa_frame');f.write(path)
        compare(str(fmt)+'-'+name,path,'warp',event=80,rid=20,layer=0)
# Structured provenance must retain uint64 IDs beyond signed JSON integer range.
for kind in ('map','update'):
    f=Fixture();rid=(1<<63)+20;event=(1<<63)+80
    desc=[10,6,1,1,103,1,0,3 if kind=='map' else 0,0 if kind=='map' else 8,0x30000 if kind=='map' else 0,0]
    f.add(rid,5,0x85,struct.pack('<QQ11IQ',123,0,*desc,21))
    f.data(21,bytes([123])*90);f.data(22,bytes((i*7+13)%256 for i in range(90)))
    payload=struct.pack('<QQiQIIIQ',0,1,0,rid,0,2,0,22) if kind=='map' else struct.pack('<QQQIBQII',0,1,rid,0,0,22,10,90)
    f.add(event,7,0x246 if kind=='map' else 0x247,payload)
    path=a.out/('uint64-'+kind+'.gpa_frame');f.write(path)
    for before in (True,False):compare('uint64-'+kind+('-before' if before else '-after'),path,'warp',event=event,rid=rid,layer=0,before=before)
save(True)
print('Checks',len(checks),'failures',sum(not x['passed'] for x in checks),flush=True)
raise SystemExit(0 if all(x['passed'] for x in checks) else 1)
