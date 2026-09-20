"""Native Texture consumer parity: storage, DDS, metadata and GPU display pixels."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--exe',type=Path,required=True)
p.add_argument('--qt-bin',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--real-only',action='store_true')
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
from frame import Frame
from assets import export_texture,export_event_texture
from formats import texture_info,subresources
from dx11 import Device
from engine import Engine
from experiments import Experiment
from validate_buffer_edits import Fixture
from validate_msaa_edits import fixture as msaa_fixture
from validate_planar_textures import fixture as planar_fixture
from image import read_png

env={k:v for k,v in os.environ.items() if k.upper() in ('SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA')}
env['PATH']=str(a.qt_bin.resolve())+os.pathsep+os.environ['WINDIR']+'/System32;'+os.environ['WINDIR']
checks=[]
def save(completed=False):
    (a.out/'validation.json').write_text(json.dumps(dict(completed=completed,passed=all(x['passed'] for x in checks),checks=checks,
        executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest()),indent=2))
def compare(name,path,rid=20,event=None,before=False,driver='hardware',sample=None,project=None,preview=True,**view):
    options=dict(mip=0,layer=0,slice=0,channel='rgba',low=0.,high=1.,typed_format=None,plane='auto');options.update(view)
    expected=a.out/(name+'-python');actual=a.out/(name+'-native');error=None;metadata=None
    with Frame(path) as frame:
        try:
            if event is None:metadata=export_texture(frame,rid,expected,preview=preview,driver=driver,**options)
            else:
                device=Device(driver)
                try:
                    engine=Engine(frame,device,experiment=Experiment(frame,project) if project else None)
                    metadata=export_event_texture(engine,rid,event,expected,after=not before,preview=preview,sample=sample,driver=driver,**options)
                finally:device.close()
        except (ValueError,RuntimeError,KeyError) as e:error=str(e)
    cmd=[str(a.exe.resolve()),'texture',str(path.resolve()),'--id',str(rid),'--out',str(actual.resolve())]
    for key,value in options.items():
        if value is not None:cmd+=['--'+key.replace('_','-'),str(value)]
    if event is not None:cmd+=['--event',str(event)]
    if before:cmd+=['--before']
    if driver=='warp':cmd+=['--warp']
    if sample is not None:cmd+=['--sample',str(sample)]
    if project:cmd+=['--experiment',str(project.resolve())]
    if not preview:cmd+=['--no-preview']
    run=subprocess.run(cmd,env=env,capture_output=True,timeout=180)
    (a.out/(name+'.log')).write_bytes(run.stdout+run.stderr)
    differences={}
    if error is not None:
        # A driver/process crash is not an explicit rejection by the consumer.
        diagnostic=None
        for line in run.stderr.decode(errors='replace').splitlines():
            try:
                value=json.loads(line)
                if isinstance(value,dict) and 'error' in value:diagnostic=value
            except ValueError:pass
        if run.returncode!=1 or not diagnostic or diagnostic.get('completed') is not False:
            differences['expected_rejection']=dict(expected=error,returncode=run.returncode,diagnostic=diagnostic)
    elif run.returncode:
        differences['native_error']=run.stderr.decode(errors='replace')[-1600:]
    else:
        got=json.loads((actual/'texture.json').read_text())
        metadata=json.loads(json.dumps(metadata))
        keys=['resource_id','value_time','width','height','depth','mips','layers','format','dimension','samples','misc',
              'sha256','subresources','selected_subresource','selected_plane','recovered_luma_only','storage_scope','uv_available',
              'source_texture','source_layout','capture_sha256','capture_plane_notice','event','pipeline_snapshot_available',
              'output_bindings','output_edit_supported','output_edit_effect','msaa','msaa_initial_data_not_applied',
              'experiment','planar_writes']
        for key in keys:
            if metadata.get(key)!=got.get(key):differences[key]=dict(expected=metadata.get(key),actual=got.get(key))
        for file in expected.iterdir():
            if file.suffix not in ('.bin','.dds','.png'):continue
            target=actual/file.name
            if not target.exists():differences[file.name]='missing';continue
            want=read_png(file)[2] if file.suffix=='.png' else file.read_bytes()
            value=read_png(target)[2] if file.suffix=='.png' else target.read_bytes()
            if want!=value:differences[file.name]=dict(expected=hashlib.sha256(want).hexdigest(),actual=hashlib.sha256(value).hexdigest(),expected_size=len(want),actual_size=len(value))
        report=json.loads((actual/'report.json').read_text())
        modules=[Path(x).name.lower() for x in report['loaded_modules']]
        if any(x.startswith(('python','gpa-','gpa_','tk8','tcl8')) or x=='renderdoc.dll' for x in modules):differences['runtime']=modules
    checks.append(dict(name=name,passed=not differences,expected_error=error,native_returncode=run.returncode,differences=differences));save()
    print(name,'PASS' if not differences else 'FAIL',flush=True)

def capture(label,kind,desc):
    path=a.out/(label+'.gpa_frame');f=Fixture();r=dict(type=kind,desc=desc)
    size=sum(s['size'] for s in subresources(texture_info(r)))
    data=bytes((i*29+17)%256 for i in range(size))
    f.add(20,5,kind,struct.pack('<QQ'+'I'*len(desc)+'Q',0,0,*desc,21));f.data(21,data);f.write(path)
    return path

if not a.real_only:
    formats=[28,29,41,42,43,54,56,57,61,62,65,71,74,77,80,83,87,91,95,98]
    for driver in ('hardware','warp'):
        for fmt in formats:
            path=capture(f'{driver}-{fmt}',0x85,[8,8,3,2,fmt,1,0,0,8,0,0])
            compare(f'{driver}-{fmt}-rgba',path,driver=driver,mip=1,layer=1)
            # The reference emits invalid HLSL for negative lows ("--0.25").
            # Exercise successful range transforms here; native negative ranges
            # have an analytical pixel regression in TextureInspectorTests.
            compare(f'{driver}-{fmt}-range',path,driver=driver,mip=2,channel='g',low=.125,high=255 if fmt in (42,43,57,62) else 1.25)
        for kind,desc,label in ((0x84,[17,4,3,28,0,8,0,0],'1d'),(0x86,[8,4,4,3,28,0,8,0,0],'3d'),
                                (0x85,[8,8,3,12,28,1,0,0,8,0,4],'cube'),(0x87,[8,4,1,1,28,1,0,0,8,0,0],'reference')):
            path=capture(driver+'-'+label,kind,desc)
            compare(driver+'-'+label,path,driver=driver,mip=0 if kind==0x87 else 1,slice=1 if kind==0x86 else 0,layer=1 if label in ('1d','cube') else 0)
        path=capture(driver+'-typeless',0x85,[8,4,1,1,27,1,0,0,8,0,0])
        for fmt in (None,28,29,30,31,32,27,41):compare(driver+'-typed-'+str(fmt),path,driver=driver,typed_format=fmt)
        for fmt in (103,104,105):
            path=a.out/(driver+f'-planar-{fmt}.gpa_frame');resource,data=planar_fixture(path,fmt)
            for plane in ('auto','y','uv'):
                for layer in (0,1):compare(driver+f'-planar-{fmt}-{plane}-{layer}',path,driver=driver,plane=plane,layer=layer)
            for vf in ((62,50) if fmt==103 else (57,36)):
                compare(driver+f'-planar-{fmt}-typed-{vf}',path,driver=driver,plane='auto',layer=1,typed_format=vf,high=255 if fmt==103 else 65535)
            compare(driver+f'-planar-{fmt}-event',path,rid=30,event=100,driver=driver,plane='uv',layer=1)
            if fmt in (104,105):
                project=a.out/(driver+f'-planar-{fmt}-override.json')
                asset=dict(data=base64.b64encode(data).decode(),sha256=hashlib.sha256(data).hexdigest())
                operation=dict(kind='texture',resource=20,asset=asset)
                document=dict(format='FloraGPA experiment 1',frame_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                              cursor=1,history=[dict(label='Explicit DXGI storage',operations=[operation])])
                project.write_text(json.dumps(document))
                compare(driver+f'-planar-{fmt}-override',path,rid=30,event=100,driver=driver,plane='uv',layer=1,project=project)
        for depth in (False,True):
            path=a.out/(driver+f'-msaa-{depth}.gpa_frame');msaa_fixture(path,depth=depth)
            for sample in (None,0,2,3):
                for layer in (0,1):compare(driver+f'-msaa-{depth}-{sample}-{layer}',path,event=100,driver=driver,sample=sample,layer=layer)
            compare(driver+f'-msaa-{depth}-initial',path,driver=driver)
            compare(driver+f'-msaa-{depth}-invalid',path,event=100,driver=driver,sample=4)
        # Invalid display selectors remain explicit even when storage export is possible.
        path=capture(driver+'-invalid',0x85,[8,4,1,1,28,1,0,0,8,0,0])
        for label,options in (('mip',dict(mip=1)),('layer',dict(layer=1)),('slice',dict(slice=1)),('plane',dict(plane='uv')),('range',dict(low=1,high=1))):
            compare(driver+'-invalid-'+label,path,driver=driver,**options)

bf=a.reference/'bf1_2026_01_21__16_53_05.gpa_frame'
for rid,mip,layer,slice in ((1156,0,0,0),(1156,3,0,0),(26351,1,4,0),(21687,0,1,0),(1418,0,0,1)):
    compare(f'bf1-{rid}-{mip}',bf,rid=rid,mip=mip,layer=layer,slice=slice)
save(True)
print('Checks',len(checks),'failures',sum(not x['passed'] for x in checks),flush=True)
raise SystemExit(0 if all(x['passed'] for x in checks) else 1)
