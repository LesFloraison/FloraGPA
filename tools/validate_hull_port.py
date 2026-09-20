"""Development-only exact hull bytecode and GPU export comparisons."""
import argparse, csv, hashlib, json, os, struct, subprocess, sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--exe',type=Path,required=True)
p.add_argument('--qt-bin',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--probe',action='store_true')
p.add_argument('--real',action='store_true')
a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
from frame import Frame
from dxbc_hull_capture import instrument
from dxbc_hull_instance import carry
from validate_hull_capture import fixture
from probe_hull_packed_identity import fixture as packed_fixture
from validate_hull_instance_uav import fixture as uav_fixture
from state import decode_state
from events import draw_event
env={k:v for k,v in os.environ.items() if k.upper() in ('SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA')}
env['PATH']=str(a.qt_bin.resolve())+os.pathsep+os.environ['WINDIR']+'/System32;'+os.environ['WINDIR']
checks=[]
def save(completed=False):
    sources=('dxbc_hull_capture.py','dxbc_hull_instance.py','hull_capture.py','post_transform.py')
    (a.out/'validation.json').write_text(json.dumps(dict(completed=completed,passed=all(c['passed'] for c in checks),checks=checks,executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),reference_sources={s:hashlib.sha256((a.reference/'standalone'/s).read_bytes()).hexdigest() for s in sources}),indent=2))
fixtures=[]
for domain in ('tri','quad','isoline'):
    for explicit in (False,True):
        for mode,factor in [('active','2'),('zero','pid==1?0:2'),('nan','pid==1?asfloat(0x7fc00000u):2')]:
            name=f'{domain}-{explicit}-{mode}';path=a.out/(name+'.gpa_frame')
            fixture(path,domain,explicit,factor,indexed=explicit,indirect=explicit,strip=explicit)
            fixtures.append((name,path,False))
for existing,full in ((True,True),(False,False),(False,True)):
    name=f'packed-{existing}-{full}';path=a.out/(name+'.gpa_frame');packed_fixture(path,existing=existing,full_input=full,explicit=True)
    fixtures.append((name,path,not existing and full))
for existing in (False,True):
    name=f'writer-{existing}';path=uav_fixture(a.out/(name+'.gpa_frame'),'vs',7,existing)
    fixtures.append((name,path,not existing))
for existing in (False,True):
    name=f'packed-implicit-{existing}';path=a.out/(name+'.gpa_frame');packed_fixture(path,existing=existing,full_input=False,explicit=False,step=3)
    fixtures.append((name,path,False))
for stage in ('hs','ds'):
    for slot in (0,7,63):
        name=f'writer-{stage}-{slot}';path=uav_fixture(a.out/(name+'.gpa_frame'),stage,slot,True)
        fixtures.append((name,path,False))
if a.probe:
    jobs=[];expected=[]
    def job(name,hs,vs=None,slot=7,patches=3,identity=None,per=3):
        target=a.out/name;target.with_suffix('.input').write_bytes(hs)
        j=dict(name=name,input=str(target.with_suffix('.input')),output=str(target.with_suffix('.dxbc')),stage='hull-identity' if vs is not None else 'hull',slot=slot,patches=patches,identity=identity,per_instance=per)
        error=None;vb=None;want=None;meta=None
        try:
            if vs is not None:vb,want,meta=carry(vs,hs)
            else:want,meta=instrument(hs,slot,patches,instance_input=identity,patches_per_instance=per)
        except (ValueError,KeyError,struct.error) as ex:error=str(ex)
        if vs is not None:
            j.update(vertex=str(target.with_suffix('.vs-input')),vertex_output=str(target.with_suffix('.vs-dxbc')));Path(j['vertex']).write_bytes(vs)
        jobs.append(j);expected.append((want,meta,vb,error))
    for name,path,reject in fixtures:
        with Frame(path) as f:
            state=decode_state(f.payload(draw_event(f,f.entries[100])['state_id']))
            hs=f.shader(f.resource(state['hs']['shader'])['data_id']);vs=f.shader(f.resource(state['vs']['shader'])['data_id'])
        for slot in (0,7,8,63):job(name+f'-u{slot}',hs,slot=slot)
        job(name+'-zero',hs,patches=0)
        job(name+'-identity',hs,vs)
        try:
            vb,hb,identity=carry(vs,hs)
            job(name+'-combined',hb,slot=63,patches=9,identity=identity)
        except ValueError:pass
    for name,opts in [('bad-slot',dict(slot=64)),('oversized',dict(patches=0xffffffff))]:job(name,hs,**opts)
    job('wrong-stage',vs)
    if a.real:
        seen=set()
        for filename in ('GF2_Exilium_2026_03_03__00_19_35.gpa_frame','bf1_2026_01_21__16_53_05.gpa_frame'):
            with Frame(a.reference/filename) as f:
                for entry in f.entries.values():
                    if entry.category!=5 or entry.type!=0x95:continue
                    raw=f.shader(f.resource(entry.id)['data_id']);digest=hashlib.sha256(raw).hexdigest()
                    if digest in seen:continue
                    seen.add(digest)
                    for slot in (0,7,8,63):job('real-'+digest[:12]+'-'+str(slot),raw,slot=slot)
    manifest=a.out/'jobs.json';manifest.write_text(json.dumps(jobs))
    run=subprocess.run([str(a.exe.resolve()),'--probe',str(manifest)],env=env,capture_output=True,timeout=120)
    assert run.returncode==0,run.stderr.decode(errors='replace')
    results=json.loads(Path(str(manifest)+'.results.json').read_text())
    for j,got,(want,meta,vb,error) in zip(jobs,results,expected):
        good=got['success']==(error is None)
        if not error:good &= got.get('metadata')==meta and Path(j['output']).read_bytes()==want and (vb is None or Path(j['vertex_output']).read_bytes()==vb)
        checks.append(dict(name=j['name'],passed=good,expected_rejection=error is not None,reference_error=error,native_error=got.get('error')))
        print(j['name'],'PASS' if good else 'FAIL',flush=True)
    save(True);sys.exit(0 if all(c['passed'] for c in checks) else 1)
from engine import Engine
from dx11 import Device
from post_transform import export_geometry
from experiments import Experiment
from validate_buffer_edits import experiment
from validate_hull_instances import fixture as multi_fixture
def numeric(value):
    try:return float(value) if value.lower() not in ('nan','inf','-inf') else value.lower()
    except ValueError:return value
def table(path):
    with path.open(encoding='utf-8-sig',newline='') as f:return [[numeric(x) for x in row] for row in csv.reader(f)]
def compare(name,path,driver,instance=None,event=100,reject=False,project=None,stream=0):
    expected=a.out/(name+'-python');actual=a.out/(name+'-native');error=None
    with Frame(path) as f:
        d=Device(driver)
        try:
            engine=Engine(f,d,experiment=Experiment(f,project) if project else None)
            try:want=export_geometry(engine,event,expected,stream=stream,stage='hs',instance=instance)
            except (ValueError,RuntimeError,KeyError) as ex:error=str(ex)
        finally:d.close()
    args=[str(a.exe.resolve()),'post-geometry',str(path),'--event',str(event),'--geometry-stage','hs','--stream',str(stream),'--out',str(actual)]
    if driver=='warp':args+=['--warp']
    if instance is not None:args+=['--instance',str(instance)]
    if project:args+=['--experiment',str(project)]
    run=subprocess.run(args,env=env,capture_output=True,timeout=180);(a.out/(name+'.log')).write_bytes(run.stdout+run.stderr)
    diff=[]
    if bool(error)!=reject:diff.append(dict(reference_error=error,expected_rejection=reject))
    if error:
        messages=[]
        for line in run.stderr.decode(errors='replace').splitlines():
            try:messages.append(json.loads(line))
            except ValueError:pass
        if run.returncode!=1 or not any(isinstance(m,dict) and m.get('completed') is False and m.get('error') for m in messages):diff.append(dict(rejection=run.returncode))
    elif run.returncode:diff.append(dict(native_error=run.stderr.decode(errors='replace')))
    else:
        got=json.loads((actual/'geometry.json').read_text())
        if want!=got:
            for key in sorted(set(want)|set(got)):
                if want.get(key)!=got.get(key):diff.append(dict(field=key,expected=want.get(key),actual=got.get(key)))
        for stem in ('vertices','patch_constants'):
            for suffix in ('.bin','.validity.bin'):
                if (actual/(stem+suffix)).read_bytes()!=(expected/(stem+suffix)).read_bytes():diff.append(dict(binary=stem+suffix))
            if table(actual/(stem+'.csv'))!=table(expected/(stem+'.csv')):diff.append(dict(csv=stem))
        if (actual/'geometry.obj').exists():diff.append(dict(obj='HS must not manufacture assembled geometry'))
        modules=[Path(m).name.lower() for m in json.loads((actual/'report.json').read_text())['loaded_modules']]
        if any(x.startswith(('python','tk8','tcl8','gpa_','gpa-')) or x in ('gpa.dll','renderdoc.dll','dx11_player.dll','dx11_playback.dll','shimd3d64.dll') for x in modules):diff.append(dict(runtime=modules))
    checks.append(dict(name=name,passed=not diff,expected_rejection=reject,reference_error=error,differences=diff));save();print(name,'PASS' if not diff else 'FAIL',flush=True)
for driver in ('hardware','warp'):
    for name,path,reject in fixtures:compare(driver+'-'+name,path,driver,reject=reject)
    for indexed,indirect,step,existing in ((False,False,0,False),(False,True,2,True),(True,False,1,True),(True,True,3,False)):
        name=f'{driver}-multi-{indexed}-{indirect}-{step}-{existing}';path=multi_fixture(a.out/(name+'.gpa_frame'),step,indexed,indirect,existing,True,True)
        for instance in (None,0,2,4):compare(name+'-'+str(instance),path,driver,instance)
    for count in (0,1,3):
        path=a.out/f'{driver}-count-{count}.gpa_frame';fixture(path,instances=count,explicit=False)
        compare(f'{driver}-count-{count}',path,driver)
        compare(f'{driver}-bad-instance-{count}',path,driver,instance=count,reject=True)
    path=a.out/f'{driver}-disabled.gpa_frame';fixture(path,instances=3,explicit=False);project=a.out/f'{driver}-disabled.json'
    with Frame(path) as f:experiment(f,project,[dict(kind='enabled',event=100,value=False)])
    compare(driver+'-disabled',path,driver,instance=1,project=project)
    compare(driver+'-bad-stream',path,driver,stream=1,reject=True)
if a.real:
    for driver in ('hardware','warp'):
        compare(driver+'-bf1',a.reference/'bf1_2026_01_21__16_53_05.gpa_frame',driver,event=11276)
save(True);sys.exit(0 if all(c['passed'] for c in checks) else 1)
