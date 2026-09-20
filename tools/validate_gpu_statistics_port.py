"""Compare native event/range statistics with the recovered Python consumer."""
import argparse,base64,csv,hashlib,json,os,struct,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference',type=Path,required=True);p.add_argument('--exe',type=Path,required=True)
p.add_argument('--qt-bin',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
p.add_argument('--real-only',action='store_true')
p.add_argument('--frames-only',action='store_true',help='With --real-only, repeat complete frames only')
p.add_argument('--repeat',type=int,default=1)
a=p.parse_args()
if a.repeat<1:p.error('--repeat must be positive')
if a.frames_only and not a.real_only:p.error('--frames-only requires --real-only')
a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
from frame import Frame
from engine import Engine
from experiments import Experiment
from devices import create_device
from gpu_statistics import sample_event,sample_range
env={k:v for k,v in os.environ.items() if k.upper() in ('SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA')}
env['PATH']=str(a.qt_bin.resolve())+os.pathsep+os.environ['WINDIR']+'/System32;'+os.environ['WINDIR']
checks=[]
def save(completed=False):
    (a.out/'validation.json').write_text(json.dumps(dict(completed=completed,passed=all(x['passed'] for x in checks),checks=checks,
        executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest()),indent=2))
def compare(label,path,driver,start,end=None,project=None,expect_error=False):
    error=None;want=None;expected=a.out/(label+'-python');actual=a.out/(label+'-native')
    with Frame(path) as frame:
        d=create_device(driver)
        try:
            e=Engine(frame,d,experiment=Experiment(frame,project) if project else None)
            try:want=sample_event(e,start,expected) if end is None else sample_range(e,start,end,expected)
            except (ValueError,RuntimeError,KeyError) as ex:error=str(ex)
        finally:d.close()
    cmd=[str(a.exe.resolve()),'statistics',str(path.resolve()),'--out',str(actual.resolve())]
    cmd+=['--event',str(start)] if end is None else ['--start-event',str(start),'--end-event',str(end)]
    if driver=='warp':cmd+=['--warp']
    if project:cmd+=['--experiment',str(project.resolve())]
    run=subprocess.run(cmd,env=env,capture_output=True,timeout=180)
    (a.out/(label+'.log')).write_bytes(run.stdout+run.stderr);diff=[]
    if bool(error)!=expect_error:diff.append(dict(reference_outcome=error,expected_rejection=expect_error))
    if error:
        messages=[]
        for line in run.stderr.decode(errors='replace').splitlines():
            try:messages.append(json.loads(line))
            except ValueError:pass
        if run.returncode!=1 or not any(isinstance(m,dict) and m.get('completed') is False and m.get('error') for m in messages):diff.append(dict(rejection=run.returncode))
    elif run.returncode:diff.append(dict(error=run.stderr.decode(errors='replace')))
    else:
        got=json.loads((actual/'statistics.json').read_text());want=json.loads(json.dumps(want))
        timing=got['timing']
        if not timing['available'] or timing['disjoint'] or timing['frequency_hz']<=0 or timing['end_tick']<timing['start_tick'] or timing['elapsed_ms']<0:
            diff.append(dict(timing=timing))
        elif abs(timing['elapsed_ms']-(timing['end_tick']-timing['start_tick'])*1000/timing['frequency_hz'])>1e-8:diff.append(dict(timing_formula=timing))
        for key in sorted(set(want)|set(got)):
            if key!='timing' and (key not in want or key not in got or want[key]!=got[key]):diff.append(dict(field=key,expected=want.get(key),actual=got.get(key)))
        def rows(folder):
            with (folder/'statistics.csv').open(encoding='utf-8-sig',newline='') as f:return {r['metric']:(r['value'].lower(),r['unit']) for r in csv.DictReader(f) if r['metric']!='elapsed_ms'}
        if rows(expected)!=rows(actual):diff.append(dict(csv=dict(expected=rows(expected),actual=rows(actual))))
        with (actual/'statistics.csv').open(encoding='utf-8-sig',newline='') as f:
            exported={r['metric']:r for r in csv.DictReader(f)}
        if float(exported['elapsed_ms']['value'])!=timing['elapsed_ms']:diff.append(dict(csv_timing=exported['elapsed_ms']))
        report=json.loads((actual/'report.json').read_text());modules=[Path(m).name.lower() for m in report['loaded_modules']]
        if any(x.startswith(('python','tk8','tcl8','gpa_','gpa-')) or x=='renderdoc.dll' for x in modules):diff.append(dict(runtime=modules))
    checks.append(dict(name=label,passed=not diff,expected_rejection=expect_error,expected_error=error,differences=diff));save();print(label,'PASS' if not diff else 'FAIL',flush=True)
root=a.reference/'output/statistics-ranges-v7';single=a.reference/'output/gpu-statistics-native-v4'
if a.real_only:
    from events import DRAW_TYPES
    for name,filename in (('gf2','GF2_Exilium_2026_03_03__00_19_35.gpa_frame'),('bf1','bf1_2026_01_21__16_53_05.gpa_frame')):
        path=a.reference/filename
        with Frame(path) as frame:
            commands=sorted(e.id for e in frame.entries.values() if e.category==7)
            draws=sorted(e.id for e in frame.entries.values() if e.category==7 and e.type in DRAW_TYPES)
            # Cover first/last work and every submission API used by each capture.
            selected={draws[0],draws[-1]}
            for kind in {frame.entries[e].type for e in draws}:
                selected.add(next(e for e in draws if frame.entries[e].type==kind))
        for driver in ('hardware','warp'):
            if not a.frames_only:
                for event in sorted(selected):compare(f'{driver}-{name}-event-{event}',path,driver,event)
            for repeat in range(a.repeat):
                suffix=f'-{repeat+1}' if a.repeat>1 else ''
                compare(f'{driver}-{name}-frame{suffix}',path,driver,commands[0],commands[-1])
    save(True);print('Checks',len(checks),'failures',sum(not c['passed'] for c in checks),flush=True)
    raise SystemExit(0 if all(c['passed'] for c in checks) else 1)
path=root/'range.gpa_frame'
def valid_raster_fixture(source):
    # These older synthetic files encode a 40-byte DESC using the later DESC2
    # type tag. Keep them untouched; produce a separate correctly tagged DESC
    # fixture. No payload bytes, shaders, queries or replay events change.
    target=a.out/'fixtures'/source.name
    if target.exists():return target
    raw=bytearray(source.read_bytes());table=struct.unpack_from('<Q',raw,0xf4)[0];fixed=[]
    with Frame(source) as frame:
        for index,e in enumerate(frame.entries.values()):
            if e.category==5 and e.type==0x10f and e.size==56:
                struct.pack_into('<H',raw,table+24*index+22,0x89);fixed.append(e.id)
    if not fixed:raise AssertionError('Expected the documented legacy rasterizer fixture tag')
    target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(raw)
    target.with_suffix('.provenance.json').write_text(json.dumps(dict(source=str(source),source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),rasterizer_desc_ids=fixed,change='Resource table type 0x10f -> 0x89, matching the existing 40-byte DESC'),indent=2))
    return target
project=a.out/'disabled.json';project.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),cursor=1,
    history=[dict(label='Disabled range',operations=[dict(kind='enabled',event=150,value=False),dict(kind='enabled',event=200,value=False)])])))
def asset(raw):return dict(data=base64.b64encode(raw).decode(),sha256=hashlib.sha256(raw).hexdigest())
def project_file(name,capture,operations):
    project=a.out/(name+'.json')
    project.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=hashlib.sha256(capture.read_bytes()).hexdigest(),cursor=1,
        history=[dict(label=name,operations=operations)])))
    return project
buffer_project=project_file('vertex-input',path,[dict(kind='buffer',event=100,resource=20,offset=8,asset=asset(struct.pack('<6f',0,0,0,0,0,0)))])
from validate_so_counts import fixture as auto_fixture
edited_autos=[]
for kind in ('doubled','clear-append','disabled-one'):
    capture=a.out/('edited-auto-'+kind+'.gpa_frame')
    operations,overrides=auto_fixture(capture,kind)
    operations += [dict(kind='shader',resource=rid,asset=asset(raw)) for rid,raw in overrides.items()]
    edited_autos.append((kind,capture,project_file('edited-auto-'+kind,capture,operations)))
for driver in ('hardware','warp'):
    for start,end in ((100,None),(200,None),(100,200),(150,160),(150,150),(200,200),(100,100)):
        compare(f'{driver}-{start}-{end}',path,driver,start,end)
    compare(driver+'-disabled-range',path,driver,100,200,project)
    compare(driver+'-disabled-event',path,driver,200,project=project)
    compare(driver+'-vertex-input-event',path,driver,100,project=buffer_project)
    compare(driver+'-vertex-input-range',path,driver,100,200,buffer_project)
    for start,end in ((150,None),(200,100),(1,200),(100,9999)):
        compare(f'{driver}-invalid-{start}-{end}',path,driver,start,end,expect_error=True)
    compare(driver+'-bad-command',root/'bad.gpa_frame',driver,100,200,expect_error=True)
    compare(driver+'-malformed-so-descriptor',single/'so-0.gpa_frame',driver,100,expect_error=True)
    compare(driver+'-malformed-predicate-descriptor',single/'predicate-1-1.gpa_frame',driver,200,expect_error=True)
    for raster in (0,1,2,3,0xffffffff):
        path_so=valid_raster_fixture(single/f'so-{raster}.gpa_frame')
        for event in (100,200):compare(f'{driver}-so-{raster}-{event}',path_so,driver,event)
    for visible in (0,1):
        for value in (0,1):
            pred=valid_raster_fixture(single/f'predicate-{visible}-{value}.gpa_frame')
            for event in (200,300):compare(f'{driver}-pred-{visible}-{value}-{event}',pred,driver,event)
    for kind in ('doubled','clear-append','disabled-one'):
        auto=root/f'auto-{kind}.gpa_frame'
        with Frame(auto) as f:events=sorted(e.id for e in f.entries.values() if e.category==7)
        compare(driver+'-auto-'+kind,auto,driver,events[0],events[-1])
    compare(driver+'-overlap',root/'overlap.gpa_frame',driver,100,200)
    for kind,capture,edits in edited_autos:
        compare(driver+'-edited-auto-'+kind+'-event',capture,driver,200,project=edits)
        compare(driver+'-edited-auto-'+kind+'-range',capture,driver,100,200,edits)
save(True);print('Checks',len(checks),'failures',sum(not c['passed'] for c in checks),flush=True)
raise SystemExit(0 if all(c['passed'] for c in checks) else 1)
