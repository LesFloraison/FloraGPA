"""Development-only parity for native VS/DS writes and GS emission exports.

Atomic allocation order is diagnostic. Preserve raw differences, compare complete
invocation records and per-invocation GS topology without assuming a global order.
"""
import argparse, collections, csv, hashlib, json, math, os, struct, subprocess, sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--exe',type=Path,required=True)
p.add_argument('--qt-bin',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--smoke',action='store_true')
p.add_argument('--real',action='store_true')
a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
from frame import Frame
from engine import Engine
from dx11 import Device
from post_transform import export_geometry
from experiments import Experiment
from probe_vertex_writes import wide
from validate_domain_writes import fixture as domain
from validate_geometry_emissions import fixture as geometry
from validate_stream_output import fixture as stream_fixture
from validate_so_passthrough import multi_fixture
from validate_hull_instance_uav import fixture as uav_fixture
from validate_buffer_edits import experiment
checks=[]
env={k:v for k,v in os.environ.items() if k.upper() in ('SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA')}
env['PATH']=str(a.qt_bin.resolve())+os.pathsep+os.environ['WINDIR']+'/System32;'+os.environ['WINDIR']
def table(path):
    with path.open(encoding='utf-8-sig',newline='') as f:return list(csv.DictReader(f))
def numeric(value):
    try:
        f=float(value)
        return value.lower() if math.isnan(f) else f
    except ValueError:return value
def canonical(folder,report):
    stride=report['stride'];values=(folder/'vertices.bin').read_bytes();flags=(folder/'vertices.validity.bin').read_bytes()
    rows=table(folder/'vertices.csv');count=report['vertex_references']
    assert len(values)==len(flags)==count*stride and len(rows)==count
    def content(row,index,exclude):
        return (tuple((k,numeric(v)) for k,v in row.items() if k not in exclude),values[index*stride:(index+1)*stride].hex(),flags[index*stride:(index+1)*stride].hex())
    if report['requested_stage']!='gs-emits':
        return sorted((content(row,i,{'record'}) for i,row in enumerate(rows)),key=repr)
    groups=collections.defaultdict(list);ordinals={}
    assert {int(row['record']) for row in rows}==set(range(count))
    for row in rows:
        inv=int(row['invocation']);index=int(row['record']);ordinal=int(row['ordinal'])
        groups[inv].append(content(row,index,{'record','invocation'}));ordinals[index]=(inv,ordinal)
    primitives=collections.defaultdict(list)
    for row in table(folder/'primitives.csv'):
        inv=int(row['invocation']);refs=[ordinals[int(row[k])] for k in ('record0','record1','record2') if row[k]!='']
        assert all(i==inv for i,o in refs)
        primitives[inv].append((int(row['stream']),tuple(o for i,o in refs)))
    return sorted(((rows,primitives[inv]) for inv,rows in groups.items()),key=repr)
def mesh(folder):
    file=folder/'geometry.obj'
    if not file.exists():return None
    positions=[];primitives=[]
    for line in file.read_text().splitlines():
        row=line.split()
        if not row or row[0].startswith('#'):continue
        if row[0]=='v':positions.append(tuple(float(v) for v in row[1:]))
        else:primitives.append((row[0],tuple(positions[int(v)-1] for v in row[1:])))
    return sorted(positions),sorted(primitives)
def save(completed=False):
    sources=('vertex_writes.py','geometry_emissions.py','so_private.py','coverage_isolated.py','post_transform.py','dxbc_vertex_writes.py','dxbc_geometry_emissions.py')
    (a.out/'validation.json').write_text(json.dumps(dict(completed=completed,passed=all(c['passed'] for c in checks),checks=checks,executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),reference_sources={s:hashlib.sha256((a.reference/'standalone'/s).read_bytes()).hexdigest() for s in sources}),indent=2))
def compare(name,path,driver,stage,event=100,stream=0,instance=None,reject=False,project=None):
    expected=a.out/(name+'-python');actual=a.out/(name+'-native');error=None
    with Frame(path) as f:
        d=Device(driver)
        try:
            e=Engine(f,d,experiment=Experiment(f,project) if project else None)
            try:want=export_geometry(e,event,expected,stream,stage,instance)
            except (ValueError,RuntimeError,KeyError) as ex:error=str(ex)
        finally:d.close()
    args=[str(a.exe.resolve()),'post-geometry',str(path.resolve()),'--event',str(event),'--geometry-stage',stage,'--stream',str(stream),'--out',str(actual)]
    if driver=='warp':args+=['--warp']
    if instance is not None:args+=['--instance',str(instance)]
    if project:args+=['--experiment',str(project)]
    run=subprocess.run(args,env=env,capture_output=True,timeout=180);(a.out/(name+'.log')).write_bytes(run.stdout+run.stderr)
    differences=[];ordering=[]
    if bool(error)!=reject:differences.append(dict(reference_error=error,expected_rejection=reject))
    if error:
        if run.returncode!=1:differences.append(dict(rejection=run.returncode))
    elif run.returncode:differences.append(dict(native_error=run.stderr.decode(errors='replace')))
    else:
        got=json.loads((actual/'geometry.json').read_text());want=json.loads(json.dumps(want))
        key={'vs-writes':'vertex_writes','ds-writes':'domain_writes','gs-emits':'geometry_emissions'}[stage]
        for field in ('sha256','full_capture_sha256','record_ordinals'):
            if want[key][field]!=got[key][field]:ordering.append(field)
        for report in (want,got):
            m=report[key];ordinals=m['record_ordinals']
            assert len(ordinals)==m['records'] and len(set(ordinals))==len(ordinals) and all(0<=i<m['full_records'] for i in ordinals)
            if instance is None:assert ordinals==list(range(m['records']))
            for field in ('sha256','full_capture_sha256','record_ordinals'):del m[field]
        if want!=got:
            for field in sorted(set(want)|set(got)):
                if want.get(field)!=got.get(field):differences.append(dict(field=field,expected=want.get(field),actual=got.get(field)))
        if canonical(expected,want)!=canonical(actual,got):differences.append(dict(records='Invocation values, written flags, identities, operations or topology differ'))
        if mesh(expected)!=mesh(actual):differences.append(dict(obj='Positions or primitive connectivity differ'))
        report=json.loads((actual/'report.json').read_text());modules=[Path(m).name.lower() for m in report['loaded_modules']]
        if any(x.startswith(('python','tk8','tcl8','gpa_','gpa-')) or x in ('renderdoc.dll','dx11_player.dll','dx11_playback.dll','shimd3d64.dll','gpa.dll') for x in modules):differences.append(dict(runtime=modules))
    checks.append(dict(name=name,passed=not differences,expected_rejection=reject,reference_error=error,allocation_order_differences=ordering,differences=differences));save();print(name,'PASS' if not differences else 'FAIL',flush=True)
cases=[]
for label,top,count,gs in [('points',1,5,False),('tail',4,8,False),('adjacency',12,9,True),('overflow',1,1200,False)]:
    path=a.out/(label+'.gpa_frame');wide(path,top,count,gs);cases.append((label,path,'vs-writes',{}))
    if a.smoke:break
for dom in (('tri',) if a.smoke else ('tri','quad','isoline')):
    path,_=domain(a.out/('ds-'+dom+'.gpa_frame'),domain=dom);cases.append(('ds-'+dom,path,'ds-writes',{}))
for label,opts in ([('triangle',{})] if a.smoke else [('point',dict(topology='point')),('line',dict(topology='line')),('triangle',{}),('wide',dict(wide=True)),('early',dict(zero=True)),('sm4',dict(profile='gs_4_0')),('sm41',dict(profile='gs_4_1'))]):
    path,_=geometry(a.out/('gs-'+label+'.gpa_frame'),**opts);cases.append(('gs-'+label,path,'gs-emits',{}))
if not a.smoke:
    from validate_geometry_instances import fixture as instances_fixture
    from validate_so_passthrough import fixture as passthrough_fixture
    from validate_geometry_emissions_edges import replace
    from shaders import compile_hlsl, chunks
    from dxbc_patch import container, instructions
    from validate_post_transform import fixture as vertex_fixture
    for indexed in (False,True):
        path=instances_fixture(a.out/f'indirect-{indexed}.gpa_frame',indexed=indexed,indirect=True)
        cases.append((f'indirect-{indexed}',path,'vs-writes',dict(instance=2)))
    path=a.out/'draw-auto.gpa_frame';stream_fixture(path)
    cases.append(('draw-auto',path,'vs-writes',dict(event=200)))
    path=a.out/'ds-so.gpa_frame';passthrough_fixture(path,profile='ds_5_0',tessellated=True)
    cases.append(('ds-so',path,'ds-writes',dict(event=150)))
    raw=compile_hlsl('struct V{float4 p:SV_Position;};[maxvertexcount(3)]void main(point float4 v[1]:SV_Position,inout TriangleStream<V> dst){V o;o.p=float4(-1,-1,.5,1);dst.Append(o);o.p=float4(-1,3,.5,1);dst.Append(o);o.p=float4(3,-1,.5,1);dst.Append(o);dst.RestartStrip();}','gs_5_0')[0]
    parts=chunks(raw);header,ops=instructions(parts['SHEX']);emits=changed=0
    for row in ops:
        if row[0]&2047 in (19,117,20,119):emits+=1
        elif emits==1 and row[0]&2047==54 and (row[1]>>12)&255==2:row[1]=(row[1]&~240)|48;changed+=1
    assert emits==3 and changed==1
    words=header+[w for row in ops for w in row];parts['SHEX']=struct.pack('<'+str(len(words))+'I',*words)
    base,_=geometry(a.out/'gs-sparse-base.gpa_frame')
    path=replace(base,a.out/'gs-sparse.gpa_frame',container(parts));cases.append(('gs-sparse',path,'gs-emits',{}))
    for profile in ('vs_4_0','vs_4_1'):
        raw=compile_hlsl('float4 main(uint id:SV_VertexID):SV_Position{return float4(id,1,2,3);}',profile)[0]
        path=vertex_fixture(a.out/(profile+'-base.gpa_frame'))
        path=replace(path,a.out/(profile+'.gpa_frame'),raw,11);cases.append((profile,path,'vs-writes',{}))
for driver in ('hardware','warp'):
    for label,path,stage,opts in cases:compare(driver+'-'+label,path,driver,stage,**opts)
    if a.smoke:continue
    for existing in (False,True):
        path=uav_fixture(a.out/f'{driver}-uav-{existing}.gpa_frame','vs',7,existing)
        compare(f'{driver}-uav-{existing}',path,driver,'vs-writes')
        compare(f'{driver}-uav-select-{existing}',path,driver,'vs-writes',instance=2,reject=not existing)
    for label,opts in [('append',dict(append=True)),('paired',dict(append=True,paired=True)),('full',dict(full_targets=True,append=True))]:
        path=a.out/f'{driver}-so-{label}.gpa_frame';stream_fixture(path,**opts)
        for event in (100,150):
            for stage in ('vs-writes','gs-emits'):compare(f'{driver}-so-{label}-{stage}-{event}',path,driver,stage,event=event)
    path=a.out/f'{driver}-multi.gpa_frame';multi_fixture(path,2)
    for stream in range(4):compare(f'{driver}-stream-{stream}',path,driver,'gs-emits',stream=stream)
    path=cases[0][1]
    compare(driver+'-selected',path,driver,'vs-writes',instance=2)
    compare(driver+'-invalid-instance',path,driver,'vs-writes',instance=3,reject=True)
    compare(driver+'-invalid-stream',path,driver,'vs-writes',stream=1,reject=True)
    project=a.out/(driver+'-disabled.json')
    with Frame(path) as f:experiment(f,project,[dict(kind='enabled',event=100,value=False)])
    compare(driver+'-disabled',path,driver,'vs-writes',project=project)
    ds_path=next(path for label,path,stage,opts in cases if label=='ds-tri')
    compare(driver+'-ds-unknown-instance',ds_path,driver,'ds-writes',instance=1,reject=True)
    gs_path=next(path for label,path,stage,opts in cases if label=='gs-triangle')
    compare(driver+'-gs-invalid-stream',gs_path,driver,'gs-emits',stream=1,reject=True)
if a.real:
    for label,filename,event,stages in [('gf2','GF2_Exilium_2026_03_03__00_19_35.gpa_frame',181,('vs-writes',)),('bf1','bf1_2026_01_21__16_53_05.gpa_frame',1415,('vs-writes',)),('bf1-tess','bf1_2026_01_21__16_53_05.gpa_frame',11276,('ds-writes',))]:
        for driver in ('hardware','warp'):
            for stage in stages:compare(f'{driver}-{label}-{stage}',a.reference/filename,driver,stage,event=event)
save(True);sys.exit(0 if all(c['passed'] for c in checks) else 1)
