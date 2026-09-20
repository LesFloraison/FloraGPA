"""Development-only native post-transform byte, metadata and export parity."""
import argparse,csv,hashlib,json,math,os,struct,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--exe',type=Path,required=True)
p.add_argument('--qt-bin',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--smoke',action='store_true')
p.add_argument('--real-only',action='store_true')
p.add_argument('--identity',action='store_true')
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
from frame import Frame
from engine import Engine
from devices import create_device
from experiments import Experiment,blob
from shaders import compile_hlsl
from post_transform import export_geometry
from validate_post_transform import fixture,VS
from validate_geometry_instances import fixture as instances
from validate_pre_raster_uav import fixture as writer
from validate_buffer_edits import graphics_fixture,experiment,operation,Fixture
from validate_stream_output import fixture as so_fixture
env={k:v for k,v in os.environ.items() if k.upper() in ('SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA')}
env['PATH']=str(a.qt_bin.resolve())+os.pathsep+os.environ['WINDIR']+'/System32;'+os.environ['WINDIR']
checks=[]
source_hashes={name:hashlib.sha256((a.reference/'standalone'/name).read_bytes()).hexdigest()
    for name in ('post_transform.py','geometry_instances.py','coverage_isolated.py','pre_raster_uav.py','vertex_identity.py','dxbc_vertex_identity.py','dxbc_hull_instance.py','dxbc_uav.py')}
def save(completed=False):
    (a.out/'validation.json').write_text(json.dumps(dict(completed=completed,passed=all(c['passed'] for c in checks),checks=checks,
        executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),reference_sources=source_hashes),indent=2))
def compare(name,path,driver='hardware',stage='final',stream=0,instance=None,event=100,project=None,reject=False):
    expected=a.out/(name+'-python');actual=a.out/(name+'-native');error=None
    with Frame(path) as frame:
        device=create_device(driver)
        try:
            engine=Engine(frame,device,experiment=Experiment(frame,project) if project else None)
            try:want=export_geometry(engine,event,expected,stream,stage,instance)
            except (ValueError,RuntimeError,KeyError) as ex:error=str(ex)
        finally:device.close()
    args=[str(a.exe.resolve()),'post-geometry',str(path.resolve()),'--event',str(event),'--geometry-stage',stage,'--stream',str(stream),'--out',str(actual.resolve())]
    if driver=='warp':args+=['--warp']
    if instance is not None:args+=['--instance',str(instance)]
    if project:args+=['--experiment',str(project.resolve())]
    run=subprocess.run(args,env=env,capture_output=True,timeout=180)
    (a.out/(name+'.log')).write_bytes(run.stdout+run.stderr);diff=[]
    if bool(error)!=reject:diff.append(dict(reference_error=error,expected_rejection=reject))
    if error:
        messages=[]
        for line in run.stderr.decode(errors='replace').splitlines():
            try:messages.append(json.loads(line))
            except ValueError:pass
        if run.returncode!=1 or not any(isinstance(m,dict) and m.get('completed') is False and m.get('error') for m in messages):diff.append(dict(rejection=run.returncode))
    elif run.returncode:diff.append(dict(native_error=run.stderr.decode(errors='replace')))
    else:
        got=json.loads((actual/'geometry.json').read_text());want=json.loads(json.dumps(want))
        for key in sorted(set(want)|set(got)):
            if key not in want or key not in got or want[key]!=got[key]:diff.append(dict(field=key,expected=want.get(key),actual=got.get(key)))
        if (expected/'vertices.bin').read_bytes()!=(actual/'vertices.bin').read_bytes():diff.append(dict(raw_bytes='different'))
        def table(root,filename='vertices.csv'):
            with (root/filename).open(encoding='utf-8-sig',newline='') as f:
                rows=list(csv.reader(f))
            def cell(x):
                try:
                    value=float(x)
                    return x.lower() if math.isnan(value) else value
                except ValueError:return x
            return [[cell(x) for x in row] for row in rows]
        if table(expected)!=table(actual):diff.append(dict(csv='different values or columns'))
        if stage=='vs-index':
            for filename in ('unique_vertices.csv','references.csv'):
                if table(expected,filename)!=table(actual,filename):diff.append(dict(file=filename,error='different values or columns'))
            unique=(actual/'unique_vertices.bin').read_bytes()
            if unique!=(expected/'unique_vertices.bin').read_bytes():diff.append(dict(unique_bytes='different'))
            with (actual/'references.csv').open(encoding='utf-8',newline='') as f:refs=list(csv.DictReader(f))
            stride=got['stride']
            reconstructed=b''.join(unique[int(row['unique_vertex'])*stride:(int(row['unique_vertex'])+1)*stride] for row in refs)
            if reconstructed!=(actual/'vertices.bin').read_bytes():diff.append(dict(reconstruction='not lossless'))
        def obj(root):
            path=root/'geometry.obj'
            if not path.exists():return None
            return [tuple([row[0]]+[float(x) for x in row[1:]]) for line in path.read_text().splitlines() if (row:=line.split()) and not row[0].startswith('#')]
        if obj(expected)!=obj(actual):diff.append(dict(obj='different geometry'))
        report=json.loads((actual/'report.json').read_text());modules=[Path(m).name.lower() for m in report['loaded_modules']]
        if any(x.startswith(('python','tk8','tcl8','gpa_','gpa-')) or x in ('renderdoc.dll','dx11_player.dll','dx11_playback.dll','shimd3d64.dll','gpa.dll') for x in modules):diff.append(dict(runtime=modules))
    checks.append(dict(name=name,passed=not diff,expected_rejection=reject,reference_error=error,differences=diff));save()
    print(name,'PASS' if not diff else 'FAIL',flush=True)
if a.identity:
    from validate_unique_ia import fixture as identity_fixture
    from validate_hull_instance_uav import fixture as identity_uav
    if a.real_only:
        for label,filename,events in (('gf2','GF2_Exilium_2026_03_03__00_19_35.gpa_frame',(181,)),('bf1','bf1_2026_01_21__16_53_05.gpa_frame',(876,1415,11276))):
            for event in events:
                from events import draw_event
                with Frame(a.reference/filename) as frame:last=draw_event(frame,frame.entries[event])['parameters'].get('instance_count',1)-1
                for driver in ('hardware','warp'):
                    for instance in (None,last):
                        compare(f'{driver}-{label}-{event}-{instance}',a.reference/filename,driver,'vs-index',event=event,instance=instance)
    else:
        for driver in ('hardware','warp'):
            for indexed in ((True,) if a.smoke else (False,True)):
                for topology in ((4,) if a.smoke else (1,2,3,4,5,10,11,12,13,35)):
                    path=a.out/f'{driver}-{indexed}-{topology}.gpa_frame'
                    identity_fixture(path,bits=32,topology=topology,indexed=indexed)
                    for instance in (None,2):compare(f'{driver}-{indexed}-{topology}-{instance}',path,driver,'vs-index',instance=instance)
            if not a.smoke:
                for width,reject in ((3,False),(4,True)):
                    source='struct V{float4 p:SV_Position;'+''.join(f'float{width} a{i}:TEXCOORD{i};' for i in range(31))+'};V main(uint id:SV_VertexID,uint inst:SV_InstanceID){V o;o.p=float4(id,inst,0,1);'+''.join(f'o.a{i}=float{width}(id,inst,{i}'+(',1' if width==4 else '')+');' for i in range(31))+'return o;}'
                    path=fixture(a.out/f'{driver}-packed-{width}.gpa_frame',source=source)
                    compare(f'{driver}-packed-{width}',path,driver,'vs-index',reject=reject)
                source='struct V{float4 p:SV_Position;uint a:FLORA_VERTEX_ID;uint b:FLORA_INSTANCE_ID;};V main(uint id:SV_VertexID,uint inst:SV_InstanceID){V o;o.p=float4(id,inst,0,1);o.a=id;o.b=inst;return o;}'
                path=fixture(a.out/f'{driver}-semantic-collision.gpa_frame',source=source)
                compare(f'{driver}-semantic-collision',path,driver,'vs-index')
                for profile in ('vs_4_0','vs_4_1'):
                    path=fixture(a.out/f'{driver}-{profile}.gpa_frame')
                    replacement=a.out/f'{driver}-{profile}.dxbc';replacement.write_bytes(compile_hlsl(VS,profile)[0])
                    project=a.out/f'{driver}-{profile}.json'
                    with Frame(path) as frame:experiment(frame,project,[dict(kind='shader',resource=10,asset=blob(replacement))])
                    compare(f'{driver}-{profile}',path,driver,'vs-index',project=project)
                for bits,step,zero,empty in ((16,0,False,False),(16,2,True,False),(32,2,False,True)):
                    path=a.out/f'{driver}-extra-{bits}-{step}-{zero}-{empty}.gpa_frame'
                    identity_fixture(path,bits=bits,step=step,zero_stride=zero,empty=empty)
                    compare(path.stem,path,driver,'vs-index')
                for existing in (False,True):
                    path=identity_uav(a.out/f'{driver}-identity-uav-{existing}.gpa_frame','vs',7,existing)
                    compare(f'{driver}-identity-uav-{existing}',path,driver,'vs-index',reject=not existing)
                path=a.out/f'{driver}-identity-errors.gpa_frame';identity_fixture(path)
                for kind,edits in (('patched-index',[operation(22,4,struct.pack('<H',6))]),('disabled',[dict(kind='enabled',event=100,value=False)])):
                    project=a.out/f'{driver}-identity-{kind}.json'
                    with Frame(path) as frame:experiment(frame,project,edits)
                    compare(f'{driver}-identity-{kind}',path,driver,'vs-index',project=project)
                for stream,instance in ((1,None),(0,4)):
                    compare(f'{driver}-identity-invalid-{stream}-{instance}',path,driver,'vs-index',stream=stream,instance=instance,reject=True)
    save(True);print('Checks',len(checks),'failures',sum(not c['passed'] for c in checks),flush=True)
    raise SystemExit(0 if all(c['passed'] for c in checks) else 1)
if a.real_only:
    from events import draw_event
    from state import decode_state
    for name,filename in (('gf2','GF2_Exilium_2026_03_03__00_19_35.gpa_frame'),('bf1','bf1_2026_01_21__16_53_05.gpa_frame')):
        path=a.reference/filename
        with Frame(path) as f:
            draws=sorted(e.id for e in f.entries.values() if e.category==7 and 0x37<=e.type<=0x3d)
            tess=next((id for id in draws if decode_state(f.payload(draw_event(f,f.entries[id])['state_id']))['hs']['shader']),None)
        for driver in ('hardware','warp'):
            for event in (draws[0],draws[-1]):compare(f'{driver}-{name}-{event}',path,driver,event=event)
            if tess:
                for stage in ('final','vs','ds'):compare(f'{driver}-{name}-tess-{tess}-{stage}',path,driver,stage,event=tess)
    save(True);print('Checks',len(checks),'failures',sum(not c['passed'] for c in checks),flush=True)
    raise SystemExit(0 if all(c['passed'] for c in checks) else 1)
base=fixture(a.out/'base.gpa_frame')
for driver in ('hardware','warp'):
    compare(driver+'-base',base,driver)
    compare(driver+'-instance',base,driver,stage='vs',instance=1)
if not a.smoke:
    for driver in ('hardware','warp'):
        for topology,count in ((1,2049),(2,4),(3,4),(5,4),(10,4),(12,6)):
            path=fixture(a.out/f'{driver}-topology-{topology}.gpa_frame',topology,count,1,0,0)
            compare(f'{driver}-topology-{topology}',path,driver)
        for factor,typename,n in ((1,'PointStream',3),(2,'LineStream',3),(3,'TriangleStream',4)):
            source='struct V{float4 p:SV_Position;uint id:TEXCOORD0;};[maxvertexcount('+str(n)+')]void main(point float4 input[1]:SV_Position,inout '+typename+'<V> dst){V v;'+''.join(f'v.p=float4({i},0,0,1);v.id={i};dst.Append(v);' for i in range(n))+'}'
            path=fixture(a.out/f'{driver}-gs-output-{factor}.gpa_frame',1,1,1,0,0,source,source='float4 main():SV_Position{return float4(0,0,0,1);}')
            compare(f'{driver}-gs-output-{factor}',path,driver)
        multi='struct V{float4 p:SV_Position;uint id:TEXCOORD0;};[maxvertexcount(10)]void main(point float4 input[1]:SV_Position,'+','.join(f'inout PointStream<V> s{i}' for i in range(4))+'){V v;'+''.join(f'v.p=float4({s},0,0,1);v.id={s};'+f's{s}.Append(v);'*(s+1) for s in range(4))+'}'
        path=fixture(a.out/f'{driver}-streams.gpa_frame',1,1,1,0,0,multi,source='float4 main():SV_Position{return float4(0,0,0,1);}')
        for stream in range(4):compare(f'{driver}-stream-{stream}',path,driver,stream=stream)
        bad=fixture(a.out/f'{driver}-zero-w.gpa_frame',source=VS.replace('inst*2,0,2','inst*2,0,0'))
        compare(f'{driver}-zero-w',bad,driver)
        replacement=a.out/f'{driver}-replacement.dxbc'
        replacement.write_bytes(compile_hlsl(VS.replace('1.5','2.5'),'vs_5_0')[0])
        for kind in ('shader','disabled','undo'):
            project=a.out/f'{driver}-{kind}.json'
            with Frame(base) as frame:
                edits=[dict(kind='enabled',event=100,value=False)] if kind=='disabled' else [dict(kind='shader',resource=10,asset=blob(replacement))]
                experiment(frame,project,edits,cursor=0 if kind=='undo' else 1)
            compare(f'{driver}-{kind}',base,driver,project=project)
        for kind in ('vs','gs','tess','overflow'):
            path=instances(a.out/f'{driver}-{kind}.gpa_frame',kind,step=2,indexed=True,indirect=True)
            for stage in (('vs','ds') if kind=='tess' else ('vs','gs') if kind in ('gs','overflow') else ('vs',)):
                compare(f'{driver}-{kind}-{stage}',path,driver,stage)
                for inst in (0,2,4):compare(f'{driver}-{kind}-{stage}-{inst}',path,driver,stage,instance=inst)
        for stage in ('vs','hs','ds','gs'):
            for slot,counter in ((1,False),(8,False),(63,True)):
                path=a.out/f'{driver}-writer-{stage}-{slot}-{counter}.gpa_frame'
                writer(path,stage,slot,counter)
                compare(f'{driver}-writer-{stage}-{slot}-{counter}',path,driver)
                if stage=='gs' and slot==63:
                    copy=Fixture();copy.records=[]
                    with Frame(path) as frame:
                        for entry in frame.entries.values():
                            raw=frame.payload(entry.id);kind=entry.type
                            if entry.id==100:
                                count,start=struct.unpack_from('<II',raw,24)
                                raw=raw[:24]+struct.pack('<4I',count,2,start,0);kind=0x3c
                            copy.add(entry.id,entry.category,kind,raw)
                    multiple=copy.write(a.out/f'{driver}-writer-gs-instances.gpa_frame')
                    compare(f'{driver}-writer-gs-instance-rejected',multiple,driver,instance=1,reject=True)
        for indirect in (False,True):
            path=graphics_fixture(a.out/f'{driver}-graphics-{indirect}.gpa_frame',indirect,True)
            compare(f'{driver}-graphics-{indirect}',path,driver)
            project=a.out/f'{driver}-graphics-edited-{indirect}.json'
            with Frame(path) as frame:
                experiment(frame,project,[operation(20,8,struct.pack('<2f',-.5,-.5)),operation(22,2,struct.pack('<3H',2,1,0))])
            compare(f'{driver}-graphics-edited-{indirect}',path,driver,project=project)
        so_path=a.out/f'{driver}-so.gpa_frame';so_fixture(so_path)
        for event in (100,200):compare(f'{driver}-so-{event}',so_path,driver,event=event)
        for stage,stream,instance in (('gs',0,None),('ds',0,None),('vs',1,None),('vs',0,2)):
            compare(f'{driver}-invalid-{stage}-{stream}-{instance}',base,driver,stage,stream,instance,reject=True)
save(True)
print('Checks',len(checks),'failures',sum(not c['passed'] for c in checks),flush=True)
raise SystemExit(0 if all(c['passed'] for c in checks) else 1)
