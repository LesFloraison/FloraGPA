"""Development-only source stack/location/ownership parity and optional DIA oracle."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
for name in ('reference','exe','qt-bin','out'):
    p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--dia-build',type=Path)
a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
from native_source_stack import compressed,statement_ranges,build,at
from native_frame_sources import location,locals_for
from native_source_variables import decode
from shader_lines import decode as lines,key
from shader_project import compile_project
from shaders import chunks
from dxbc_patch import container,instructions
from validate_gs_source_lines import PROJECT
from validate_gs_source_stack import nested
from probe_hs_source_scopes import project as hull_project
from validate_ds_checkpoints import project as domain_project
from validate_sdbg import legacy

jobs=[];expected=[];dia_jobs={}
def job(name,op,oracle,**kwargs):
    jobs.append(dict(name=name,op=op,**kwargs))
    try:expected.append(dict(success=True,value=json.loads(json.dumps(oracle()))))
    except (ValueError,KeyError,TypeError,IndexError) as exc:expected.append(dict(success=False,error=str(exc)))

def stack(name,raw,symbols=None,source=None,dia=False):
    path=a.out/(name+'.dxbc');path.write_bytes(raw)
    code=chunks(raw);code=code.get('SHEX',code.get('SHDR'))
    queries=[dict(offset=i,depth=depth) for depth in (0,1) for i in range(0,len(code)+4,4)]
    def oracle():
        s=copy.deepcopy(symbols) if symbols is not None else decode(raw)
        result=build(raw,s,source)
        return dict(model=result,symbols=s,queries=[dict(stack=at(result,q['offset'],q['depth']),
            locations={f['id']:location(result,f['id'],q['offset']) for f in result['frames']}) for q in queries])
    extra={}
    if symbols is not None:extra['symbols']=symbols
    if source is not None:extra['source']=source
    job(name,'source-stack',oracle,input=str(path),queries=queries,**extra)
    if dia and a.dia_build:
        pdb=a.out/(name+'.pdb');pdb.write_bytes(chunks(raw)['SPDB'])
        info=json.loads((a.dia_build/'build.json').read_text())
        assert hashlib.sha256((a.dia_build/'DiaInlineProbe.exe').read_bytes()).hexdigest()==info['exe_sha256']
        assert hashlib.sha256(Path(info['dia_dll']).read_bytes()).hexdigest()==info['dia_sha256']
        run=subprocess.run([str(a.dia_build/'DiaInlineProbe.exe'),info['dia_dll'],str(pdb),str(len(code))],
                           capture_output=True,text=True,check=True,timeout=30)
        (a.out/(name+'.dia.txt')).write_text(run.stdout,encoding='utf-8')
        dia_jobs[len(jobs)-1]=(run.stdout,lines(raw),info)

projects=[('single',PROJECT),('nested5',nested(5)),('nested1',nested(1))]
included=nested(5)
included['files'][2]['text']=included['files'][2]['text'].replace('uint leaf(uint v)\n{\n    return v+7;\n}', '#include "leaf.hlsl"')
included['files'].append(dict(name='leaf.hlsl',text='uint leaf(uint v)\n{\n uint result=v;\n [loop]for(uint i=0;i<2;i++)\n {\n  result+=i+3;\n }\n return result;\n}\n'))
projects.append(('nested-include-loop',included))
for flags in (1,5,2049):projects.extend([('hs-'+str(flags),hull_project(flags)),('ds-'+str(flags),domain_project(flags=flags))])
for name,project in projects:
    raw,_=compile_project(project);stack(name,raw,dia=name.startswith('nested'))
raw,_=compile_project(nested(5));symbols=decode(raw);source=lines(raw)
stack('stripped',container({k:v for k,v in chunks(raw).items() if k!='SPDB'}))
stack('legacy',legacy('float4 pixelEntry():SV_Target {return float4(0,1,0,1);}\n'),symbols=dict(format='SDBG assignments'))
for name,edit in [
    ('wrong-shader',lambda s:s.update(shader_sha256='0'*64)),
    ('no-locals',lambda s:s.update(status='no_local_symbols',variables=[])),
    ('no-scopes',lambda s:s.update(scopes=[])),
    ('unavailable',lambda s:s.update(status='unavailable')),
    ('recursive-parent',lambda s:s['scopes'][0].update(parent=s['scopes'][0]['id'])),
    ('missing-parent',lambda s:s['scopes'][1].update(parent='missing'))]:
    altered=copy.deepcopy(symbols);edit(altered);stack(name,raw,symbols=altered)
for name,edit in [
    ('wrong-source',lambda s:s.update(shader_sha256='0'*64)),
    ('missing-source',lambda s:s.update(status='unavailable')),
    ('missing-module',lambda s:s.update(scope_modules=[])),
    ('missing-inlinees',lambda s:s['scope_modules'][0].update(inlinees=[])),
    ('invalid-inlinees',lambda s:s['scope_modules'][0].update(inlinee_issues=['Invalid inlinee record'])),
    ('unknown-checksums',lambda s:s['scope_modules'][0].update(checksums={})),
    ('ambiguous-location',lambda s:s['scope_modules'][0]['base_locations'].extend([
        dict(s['scope_modules'][0]['base_locations'][0],file=None,statement=False),
        dict(s['scope_modules'][0]['base_locations'][0],line_start=1,line_end=1,statement=False)]))]:
    altered=copy.deepcopy(source);edit(altered);stack(name,raw,source=altered)
for text in ('0','e0','0001','0300','0c0400','0201','0c0300','03ff', '0g', '0 1', ' 03 00 04 04 '):
    altered=copy.deepcopy(symbols);next(s for s in altered['scopes'] if s['kind']=='inline')['annotations']=text
    stack('annotation-'+str(len(jobs)),raw,symbols=altered)
for separator in ['\n','\r','\r\n','\v','\f','\x1c','\x1d','\x1e','\x85','\u2028','\u2029']:
    for trailing in (False,True):
        altered=copy.deepcopy(source)
        for f in altered['files']:f['text']=separator.join(f['text'].splitlines())+(separator if trailing else '')
        stack('source-separator-'+str(len(jobs)),raw,source=altered)

rng=random.Random(49383)
for first in range(256):
    for size in (1,2,3,4):
        data=bytes([first])+rng.randbytes(size-1)
        job(f'compressed-{first}-{size}','inline-compressed',lambda:compressed(data,0),bytes=list(data))
for offset in (0,1,9):
    job('compressed-offset-'+str(offset),'inline-compressed',lambda:compressed(b'',offset),bytes=[],offset=offset)
def packed(value):
    if value<128:return bytes([value])
    if value<16384:return (value|0x8000).to_bytes(2,'big')
    return (value|0xc0000000).to_bytes(4,'big')
def ranges(name,data,boundaries,base=0):
    for details in (False,True):
        job(name+'-'+str(details),'inline-ranges',lambda:statement_ranges(data,base,set(boundaries),details),
            bytes=list(data),base=base,boundaries=boundaries,details=details)
for n in range(600):
    ops=[]
    for i in range(rng.randrange(1,12)):
        op=rng.randrange(14);ops.extend([op,rng.randrange(33)])
        if op==12:ops.append(rng.randrange(33))
    ranges('grammar-'+str(n),b''.join(packed(v) for v in ops),list(range(0,128,4)))
for n in range(300):
    # Successful multiple statement ranges, file changes, signed line deltas and padding.
    ops=[5,rng.randrange(4),6,rng.randrange(64),7,rng.randrange(4),3,0]
    count=rng.randrange(1,9)
    for i in range(count-1):ops.extend([3,4])
    ops.extend([4,4,0,0])
    ranges('valid-statements-'+str(n),b''.join(packed(v) for v in ops),list(range(0,4*(count+1),4)))
for scope in symbols['scopes']:
    if scope['kind']!='inline':continue
    data=bytes.fromhex(scope['annotations']);code=chunks(raw)['SHEX'];boundaries={len(code)};offset=8
    for op in instructions(code)[1]:boundaries.add(offset);offset+=len(op)*4
    base=next(s['code_start'] for s in symbols['scopes'] if s['kind']=='function')
    ranges('real-'+scope['id'],data,sorted(boundaries),base)
    for end in range(len(data)):ranges(f'real-{scope["id"]}-truncate-{end}',data[:end],sorted(boundaries),base)
for name,data in [('overflow', b'\x08\x00'+(b'\x03'+packed(0x1fffffff))*9),
                  ('bad-kind',b'\x08\x02'),('reset-pending',b'\x03\x00\x01\x00'),
                  ('kind-pending',b'\x03\x00\x08\x00'),('separated',b'\x02\x01')]:
    ranges(name,data,[0,4,8])

base=dict(status='available',frames=[dict(id='main',name='main',kind='function',parent=None,ranges=[dict(start=8,end=40)]),
                                    dict(id='inline',name='leaf',kind='inline',parent='main',ranges=[dict(start=12,end=24)])])
for name,edit in [('valid',lambda m:None),('cycle',lambda m:m['frames'][0].update(parent='inline')),
                  ('inactive-parent',lambda m:m['frames'][0].update(ranges=[])),
                  ('siblings',lambda m:m['frames'].append(dict(m['frames'][1],id='sibling'))),
                  ('missing-parent',lambda m:m['frames'][1].update(parent='missing')),
                  ('hs-no-phase',lambda m:m.update(hs_phases=[])),
                  ('hs-no-frame',lambda m:m.update(hs_phases=[dict(id=0,start=8,end=40,frame_ids=[])])),
                  ('hs-inline',lambda m:m.update(hs_phases=[dict(id=0,kind='fork',start=8,end=40,frame_ids=['inline'])],scope_semantics='phase_local'))]:
    model=copy.deepcopy(base);edit(model)
    for offset in (0,8,11,12,16,23,24,39,40):
        for depth in (0,1):job(f'at-{name}-{offset}-{depth}','stack-at',lambda:at(model,offset,depth),model=model,offset=offset,depth=depth)
for parents in [(None,'main','block'),(None,'main','main'),(None,'cycle','block')]:
    scopes=[dict(id='main',parent=parents[0],kind='function'),dict(id='block',parent=parents[1],kind='block'),
            dict(id='inline',parent=parents[2],kind='inline'),dict(id='cycle',parent='block',kind='block')]
    values=[dict(scope_id=s['id'],value=i) for i,s in enumerate(scopes)]+[dict(scope_id=None,value=99)]
    syms=dict(scopes=scopes)
    for ident in ('main','inline','block','missing'):
        job('locals-'+str(len(jobs)),'frame-locals',lambda:locals_for(syms,values,ident),symbols=syms,values=values,frame=ident)
for rows in [[],[dict(start=8,end=16,file=0,line_start=1,line_end=2)],
             [dict(start=8,end=16,file=0,line_start=1,line_end=2),dict(start=8,end=16,file=0,line_start=2,line_end=3)]]:
    model=dict(frames=[dict(id='main',locations=rows)])
    for offset in (7,8,15,16):job('location-'+str(len(jobs)),'frame-location',lambda:location(model,'main',offset),model=model,frame='main',offset=offset)

env=dict(os.environ);env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH']
manifest=a.out/'jobs.json';manifest.write_text(json.dumps(jobs),encoding='utf-8')
(a.out/'expected.json').write_text(json.dumps(expected),encoding='utf-8')
subprocess.run([str(a.exe.resolve()),'--probe',str(manifest)],env=env,check=True,timeout=120)
actual=json.loads(Path(str(manifest)+'.results.json').read_text());assert len(actual)==len(expected)
def diagnostics(value):
    if isinstance(value,dict):return {k:([bool(x) for x in v] if k in ('issues','location_issues') else diagnostics(v)) for k,v in value.items()}
    if isinstance(value,list):return [diagnostics(v) for v in value]
    return value
checks=[]
for j,want,got in zip(jobs,expected,actual):
    exact=want['success']==got['success'] and (not want['success'] or want['value']==got['value'])
    passed=want['success']==got['success'] and (not want['success'] or diagnostics(want['value'])==diagnostics(got['value']))
    checks.append(dict(name=j['name'],passed=passed,exact=exact))
    if not passed:print('FAIL',j['name'],flush=True)
for index,(text,source,info) in dia_jobs.items():
    got=actual[index]['value'];queries={q['offset']:v for q,v in zip(jobs[index]['queries'],got['queries']) if not q['depth']}
    oracle={};identities={};compared=0;valid=True
    for line in text.splitlines():
        fields=line.split('\t');offset,ident=int(fields[0]),fields[1]
        oracle.setdefault(offset,{}).setdefault(ident,[]).append(dict(name=fields[2],first=int(fields[3]),last=int(fields[4]),statement=bool(int(fields[7])),file=key(fields[8])))
    for offset,groups in oracle.items():
        query=queries[offset];frames=query['stack']['frames'];ordered=list(groups.items());ordered=ordered[:1]+list(reversed(ordered[1:]))
        valid &= len(frames)==len(ordered)
        for (ident,rows),frame in zip(ordered,frames):
            valid &= all(r['name']==frame['name'] for r in rows)
            if ident in identities:valid &= identities[ident]==frame['id']
            identities[ident]=frame['id']
            preferred=[r for r in rows if r['statement']==(frame['kind']=='inline')] or rows
            expected_locations={(r['file'],r['first'],r['last']) for r in preferred}
            loc=query['locations'][frame['id']]
            valid &= loc is not None and (key(source['files'][loc['file']]['name']),loc['line_start'],loc['line_end']) in expected_locations
            compared+=1
    valid &= compared>100 and len(identities)==len(set(identities.values()))==len(got['model']['frames'])
    checks.append(dict(name=jobs[index]['name']+'-DIA',passed=bool(valid),locations=compared,oracle_build=info))
result=dict(passed=all(c['passed'] for c in checks),count=len(checks),exact=sum(c.get('exact',False) for c in checks),
    queries=sum(len(j.get('queries',[])) for j in jobs),checks=checks,
    executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),
    sources={s:hashlib.sha256((a.reference/'standalone'/s).read_bytes()).hexdigest() for s in ('native_source_stack.py','native_frame_sources.py','native_hs_scopes.py')})
(a.out/'validation.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in result.items() if k not in ('checks','sources')}),flush=True)
raise SystemExit(0 if result['passed'] else 1)
