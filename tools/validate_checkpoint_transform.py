"""Development-only full DXBC and metadata comparisons for native checkpoints/traces."""
import argparse
import copy
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
p.add_argument('--real',action='store_true')
a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
from dxbc_gs_checkpoint import instrument,program
from dxbc_hull_phases import layout
from dxbc_hull_capture import op,dst,src,imm
from dxbc_patch import container
from shaders import compile_hlsl,chunks
from frame import Frame
from native_invocation_selector import keys,FORMAT
from validate_gs_indexable import make_fixture as array_fixture
from validate_gs_calls import make_fixture as call_fixture
from validate_ds_checkpoints import fixture as ds_fixture
from validate_hs_checkpoints import fixture as hs_fixture,join_fixture

jobs=[];expected=[];shader_count=0
def job(name,raw,stage,checkpoint='trace',phase=None,slot=63,capacity=17,selector=None):
    base=a.out/name
    base.with_suffix('.input').write_bytes(raw)
    j=dict(name=name,op='instrument',input=str(base.with_suffix('.input')),output=str(base.with_suffix('.native.dxbc')),
           stage=stage,checkpoint=checkpoint,phase=phase,slot=slot,capacity=capacity,selector=selector)
    try:
        patched,meta=instrument(raw,slot,capacity,checkpoint,stage,selector,phase)
        base.with_suffix('.reference.dxbc').write_bytes(patched)
        base.with_suffix('.reference.json').write_text(json.dumps(meta,indent=2),encoding='utf-8')
        want=dict(success=True,metadata=meta,digest=hashlib.sha256(patched).hexdigest())
    except (ValueError,KeyError,IndexError,struct.error) as error:
        want=dict(success=False,error=str(error))
    jobs.append(j);expected.append(want)

def shader(name,raw,stage,exhaustive=True):
    global shader_count
    shader_count+=1
    _,_,_,_,rows,split,catalog=program(raw,stage)
    phases=layout(rows) if stage=='hs' else [None]
    for phase in phases:
        pid=None if phase is None else phase['id']
        suffix='' if phase is None else '-phase'+str(pid)
        for slot in (0,7,8,63):
            job(name+suffix+'-trace-u'+str(slot),raw,stage,phase=pid,slot=slot)
        job(name+suffix+'-capacity1',raw,stage,phase=pid,capacity=1)
        _,meta=instrument(raw,63,17,'trace',stage,hs_phase=pid)
        terms=[dict(name=n,component=c,bits=(0x7fc12345+i*19)&0xffffffff) for i,(n,c) in enumerate(keys(meta['register_slots'],meta['known_inputs']))]
        if terms:
            selector=dict(format=FORMAT,shader_stage=stage,shader_sha256=hashlib.sha256(raw).hexdigest(),inputs=terms,match_policy='unique')
            if phase is not None:selector['hs_phase']={k:phase[k] for k in ('id','kind')}
            job(name+suffix+'-selector',raw,stage,phase=pid,selector=selector)
            selector=copy.deepcopy(selector);selector['match_policy']='all';selector['inputs'].reverse()
            job(name+suffix+'-selector-all',raw,stage,phase=pid,selector=selector)
        allowed=[r for r in catalog if r['checkpoint_allowed'] and (phase is None or r['hs_phase']==pid)]
        for row in allowed if exhaustive else allowed[:1]+allowed[-1:]:
            job(name+suffix+'-checkpoint'+str(row['token']),raw,stage,checkpoint=row['token'],phase=pid,capacity=3)
        job(name+suffix+'-bad-capacity',raw,stage,phase=pid,capacity=0)
        job(name+suffix+'-bad-slot',raw,stage,phase=pid,slot=64)
        job(name+suffix+'-too-large',raw,stage,phase=pid,capacity=0xffffffff)
    job(name+'-declaration-token',raw,stage,checkpoint=0)
    job(name+'-outside-token',raw,stage,checkpoint=len(rows))
    if stage=='hs':job(name+'-missing-phase',raw,stage)
    else:job(name+'-wrong-phase',raw,stage,phase=0)

gs_source='''struct V{float4 p:SV_Position;uint4 value:TEXCOORD0;};
[maxvertexcount(3)]void main(point float4 input[1]:SV_Position,uint prim:SV_PrimitiveID,inout PointStream<V> dst){
float4 a[5];[loop]for(uint i=0;i<5;i++)a[i]=float4(i+.25,prim+.5,input[0].z,42);
[loop]for(uint n=0;n<3;n++){V o;o.p=input[0];o.value=asuint(a[(prim+n)%5]);dst.Append(o);dst.RestartStrip();}}'''
for profile in ('gs_4_0','gs_4_1','gs_5_0'):
    shader('hlsl-'+profile,compile_hlsl(gs_source,profile)[0],'gs')
    folder=a.out/('array-'+profile);path,tags=array_fixture(folder,profile)
    with Frame(path) as f:raw=f.shader(81)
    shader('indexed-'+profile,raw,'gs')
for depth in (None,1,31,32,33):
    folder=a.out/('calls-'+str(depth));path=call_fixture(folder,depth)
    with Frame(path) as f:raw=f.shader(81)
    shader('calls-'+str(depth),raw,'gs',depth in (None,1))
for domain in ('tri','quad','isoline'):
    _,_,raw=ds_fixture(a.out/('ds-'+domain+'.gpa_frame'),domain=domain)
    shader('ds-'+domain,raw,'ds')
    for explicit in (False,True):
        _,raw,_=hs_fixture(a.out/f'hs-{domain}-{explicit}.gpa_frame',domain=domain,explicit=explicit,flags=5)
        shader(f'hs-{domain}-{explicit}',raw,'hs')
_,raw,_=join_fixture(a.out/'hs-join.gpa_frame')
shader('hs-join',raw,'hs')
_,hraw,_=hs_fixture(a.out/'hs-relative.gpa_frame',explicit=True,flags=5)
_,hparts,hkey,hheader,hrows,_,_=program(hraw,'hs')
phase=layout(hrows)[0]
for representation in (2,3):
    modified=copy.deepcopy(hrows)
    temp=next(r for r in modified[phase['start']+1:phase['split']] if r[0]&2047==104)
    index_register=temp[1];temp[1]+=1
    from dxbc_gs_checkpoint import operands_of
    chosen=None
    for i in range(phase['split'],phase['end']):
        args=operands_of(modified[i])[1]
        if args and (modified[i][args[0][0]]>>12)&255==2:
            chosen=i;break
    assert chosen is not None
    row=modified[chosen];start,end=operands_of(row)[1][0];reg=row[start+1]
    dest=[(row[start]&~(7<<22))|(representation<<22),*([reg] if representation==3 else []),*src(index_register,0)]
    row=row[:start]+dest+row[end:];row[0]=(row[0]&~0x7f000000)|(len(row)<<24)
    modified[chosen:chosen+1]=[op(54,dst(index_register,1),imm(0 if representation==3 else reg)),row]
    words=hheader+[w for r in modified for w in r];words[1]=len(words)
    nextparts=dict(hparts);nextparts[hkey]=struct.pack('<'+'I'*len(words),*words)
    for stale in ('STAT','SDBG','SPDB','ILDB','SRCI','ILDN'):nextparts.pop(stale,None)
    shader('hs-relative-'+str(representation),container(nextparts),'hs',False)

# Authored mutation cases exercise destination staging, relative output addressing,
# UAV collision and explicit guards independently of what FXC happens to emit.
with Frame(a.out/'array-gs_5_0/indexed.gpa_frame') as f:base=f.shader(81)
_,parts,key,header,rows,split,_=program(base)
decl=rows[:split]
def changed(declarations,body):
    nextparts=dict(parts)
    words=header+[w for row in declarations+body for w in row];words[1]=len(words)
    nextparts[key]=struct.pack('<'+'I'*len(words),*words)
    return container(nextparts)
from dxbc_indexable import operand as x
for opcode in (38,77,78,81,132,133,142):
    from dxbc_gs_checkpoint import COUNTS
    body=[op(54,dst(0),imm(0)),op(opcode,dst(0,1),x(0,0,mask=1,relative=src(0,0)),*[imm(7) for _ in range(COUNTS[opcode]-2)]),op(62)]
    raw=changed(decl,body)
    job(f'dependent-result-{opcode}',raw,'gs')
    if opcode in (132,133):
        body[1]=op(opcode,dst(0,1),x(0,0,mask=2,relative=src(0,0)),imm(7),imm(9))
        job(f'dependent-result-{opcode}-mask-mismatch',changed(decl,body),'gs')
for name,body,extra in [
    ('read-output',[op(54,dst(0),[0x102e46,0]),op(62)],[]),
    ('missing-label',[op(4,[0x10a000,999]),op(62)],[]),
    ('recursive',[op(4,[0x10a000,1]),op(62),op(44,[0x10a000,1]),op(4,[0x10a000,1]),op(62)],[]),
    ('interface',[op(120,[0,0,0]),op(62)],[]),
    ('feedback',[op(218),op(62)],[]),
    ('no-ret',[op(54,dst(0),imm(0))],[]),
    ('static-oob',[op(54,dst(0),x(0,4,swizzle=0xe4)),op(62)],[]),
    ('relative-index',[op(54,dst(0),imm(99)),op(54,x(0,0,relative=src(0,0)),imm(1)),op(62)],[]),
    ('collision',[op(62)],[op(157,[0x11e000,63])]),
    ('high-free-slot',[op(62)],[op(157,[0x11e000,7])]),
    ('temp-limit',[op(62)],[op(104,[4096])])]:
    job('guard-'+name,changed(decl+extra,body),'gs')

real_count=0
if a.real:
    seen=set()
    for filename in ('GF2_Exilium_2026_03_03__00_19_35.gpa_frame','bf1_2026_01_21__16_53_05.gpa_frame'):
        with Frame(a.reference/filename) as f:
            for entry in f.entries.values():
                if entry.category!=5 or entry.type not in (0x91,0x94,0x95):continue
                raw=f.shader(f.resource(entry.id)['data_id']);digest=hashlib.sha256(raw).hexdigest()
                if digest in seen:continue
                seen.add(digest);stage={0x91:'gs',0x94:'ds',0x95:'hs'}[entry.type]
                try:program(raw,stage)
                except ValueError:continue
                real_count+=1;shader('real-'+stage+'-'+digest[:12],raw,stage,False)

manifest=a.out/'jobs.json';manifest.write_text(json.dumps(jobs),encoding='utf-8')
(a.out/'expected.json').write_text(json.dumps(expected,indent=2),encoding='utf-8')
env={k:v for k,v in os.environ.items() if k.upper() in ('SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA')}
env['PATH']=str(a.qt_bin.resolve())+os.pathsep+os.environ['WINDIR']+'/System32'
run=subprocess.run([str(a.exe.resolve()),'--probe',str(manifest)],env=env,capture_output=True,timeout=180)
(a.out/'native.log').write_bytes(run.stdout+run.stderr)
assert run.returncode==0,run.stderr.decode(errors='replace')
actual=json.loads(Path(str(manifest)+'.results.json').read_text());assert len(actual)==len(jobs)
checks=[]
for j,want,got in zip(jobs,expected,actual):
    good=j['name']==got['name'] and got['success']==want['success']
    if want['success']:
        good &= got.get('value')==want['metadata'] and Path(j['output']).exists() and hashlib.sha256(Path(j['output']).read_bytes()).hexdigest()==want['digest']
    checks.append(dict(name=j['name'],passed=good,expected_rejection=not want['success'],reference_error=want.get('error'),native_error=got.get('error')))
    if not good:print('FAIL',j['name'],got.get('error'),want.get('error'),flush=True)
report=dict(completed=True,passed=all(c['passed'] for c in checks),checks=checks,shader_count=shader_count,real_shader_count=real_count,
            executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),reference_sha256=hashlib.sha256((a.reference/'standalone/dxbc_gs_checkpoint.py').read_bytes()).hexdigest())
(a.out/'validation.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(f"{sum(c['passed'] for c in checks)}/{len(checks)} passed; {sum(c['expected_rejection'] for c in checks)} expected rejections; {shader_count} shaders ({real_count} real)")
sys.exit(0 if report['passed'] else 1)
