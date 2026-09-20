"""Development-only debugger expression and native source environment parity."""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import random
import struct
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
for name in ('reference','exe','qt-bin','out'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--captures',type=Path)
a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
from debug_expression import Expression,ExpressionError
from native_debug_expression import NativeEnvironment
from native_sdbg_expression import SdbgEnvironment
from native_source_variables import decode,resolve
from native_source_stack import build,at
from gs_checkpoint import headers,register_values

jobs=[];expected=[]
def record(name,kind,values,scope='local',**kw):return dict(name=name,type=kind,values=values,scope=scope,status='available',columns=len(values),**kw)
records=[record('i','uint',[2]),record('pair.uv','float',[49.5,4.]),record('pair.exact','uint',[16777219]),
         record('pair.signedValue','int',[-7]),record('a[1]','int',[8]),record('i','int',[100],'global')]
def job(text,rows=records,parse_only=False,**kwargs):
    jobs.append(dict(name='expression-'+str(len(jobs)),op='debug-expression',expression=text,records=rows,parse_only=parse_only,**kwargs))
    try:
        expr=Expression(text)
        if parse_only:value=expr.text
        else:
            env=kwargs.get('environment')
            if env=='native':v=NativeEnvironment(kwargs['symbols'],kwargs['values'],kwargs['offset'],kwargs['frame']).expression(expr)
            elif env=='sdbg':v=SdbgEnvironment(kwargs['symbols'],kwargs['values'],kwargs['offset']).expression(expr)
            else:v=expr.evaluate(rows)
            value=dict(type=v.kind,values=[dict(bits=int.from_bytes(struct.pack('<d',x),'little')) if isinstance(x,float) else dict(value=x) for x in v.values],text=v.text(),truth=v.truth() if len(v.values)==1 else None)
        expected.append(dict(success=True,value=value))
    except (ExpressionError,ValueError,KeyError,OverflowError) as exc:expected.append(dict(success=False,error=str(exc)))

for text in ['i','pair.uv.yx','a[i-1u]+int(pair.uv[1])','1+2*3==7 && (1+2)*3==9','pair.exact+1u',
             '0xffffffffu+1u','-7/3','-7%3','((pair.exact & 255u)<<2u)^1u','-8>>1u','uint(-8)>>1',
             'float(16777216)+1','double(16777216)+1','all(float2(1,2)+2==float2(3,4))','any(pair.uv>40)',
             'max(abs(-7),min(9,8))','bool(0)','false && missing>0','true || missing>0','i==2?pair.exact:missing',
             'false?missing:true?1:2','missing','pair.uv && true','pair.uv?1:0','texture.Load(0)',
             'i=3','i++','++i','--i','__import__(1)','[1,2]','pair.__class__','i;2','i**2','i//2',
             'pair.uv.z','pair.uv[2]','pair.uv[-1]','pair.uv[1.0]','1/0','1<<32','1<<-1',
             'float2(1,2)+float3(1,2,3)','int(1e38)','1'*2049,'('*50+'1'+')'*50,'+'.join(['1']*150),
             '4294967296u','0x100000000','0xffffffff*0xffffffff','abs(-2147483648)','-2147483648/-1',
             'float(1e999)','int(1e999)','float(1e-999)','all()','abs(1,2)','float()','float2(1,2,3)',
             'bool2(0,1)','float2(1,2).rg','float3(1,2,3).zyx','1.2u','1e2u','.2f','1F','0xff',
             '١+２','𝟙 + 𝟚','１.２f + ٣e1','\x1c 1 \x1f + 2 \x85','', ' '*2049,
             '1<2<3','1|2&&0','1?2:3?4:5','1+(2).x','float2(1,2)[1]','~true','true+true']:
    job(text)
for rows in [[dict(records[0],status=s)] for s in ('undefined','partial','mapping_mismatch','unavailable')]+[
    [dict(records[0],columns=2)],[records[0],records[0]],
    [record('pair.uv','float',[1,2]),record('pair.exact','uint',[7],'global')],
    [record('large','uint64',[18446744073709551615])],
    [record('i','int',['wrong'])],[record('i','float',[])]]:
    for text in ('i','pair.exact','large'):job(text,rows)
for kind in ('half','int8','int16','uint8','uint16','enum','double','bool','int64','uint64','unknown'):
    job('x+1',[record('x',kind,[7])])
rng=random.Random(790123)
ops=['+','-','*','/','%','&','|','^','<<','>>','==','!=','<','<=','>','>=']
for i in range(3500):
    left,right=rng.getrandbits(32),rng.getrandbits(32)
    atext=f'{left}u' if rng.randrange(2) else f'int({left}u)'
    btext=f'{right}u' if rng.randrange(2) else f'int({right}u)'
    op=rng.choice(ops)
    if op in ('<<','>>'):btext=str(rng.randrange(-2,34))
    job(f'({atext}) {op} ({btext})',[])
for i in range(1500):
    left,right=[struct.unpack('<f',rng.randbytes(4))[0] for _ in range(2)]
    if not all(math.isfinite(v) for v in (left,right)):continue
    kind=rng.choice(['float','double']);op=rng.choice(['+','-','*','/','%','==','<','>='])
    job('x '+op+' y',[record('x',kind,[left]),record('y',kind,[right])])
for count in range(1,5):
    for kind in ('bool','int','uint','float','double'):
        name=kind+(str(count) if count>1 else '')
        expr=name+'('+','.join(str(i+1) for i in range(count))+')'
        for text in [expr,'!'+expr,'-'+expr,'~'+expr,'abs('+expr+')','all('+expr+')','any('+expr+')',
                     'max('+expr+',2)','min('+expr+',2)',expr+'+2',expr+'==2',expr+'.xxxx',expr+'[0]']:
            job(text,[])
for c in range(0x110000):
    if chr(c).isspace():job(chr(c)+'1'+chr(c)+'+'+chr(c)+'2'+chr(c),[])
for length in (39,40,41,42):job('('*length+'1'+')'*length,[],parse_only=True)
for count in (127,128,129):job('+'.join(['1']*count),[],parse_only=True)
for prefix in ['0x','0X','0xg','.', '1e','1e+','01', '1f','1u','0XAfU']:
    job(prefix,[],parse_only=True)

scopes=[dict(id='main',parent=None,kind='function'),dict(id='block',parent='main',kind='block',code_start=4,code_end=8),
        dict(id='inline',parent='block',kind='inline')]
variables=[dict(id='outer',name='x',type={}),dict(id='inner',name='x',type={}),dict(id='arg',name='x',type={})]
values=[dict(variable_id=v['id'],scope_id=s['id'],name='x',type='uint',status='available',bits=n)
        for v,s,n in zip(variables,scopes,(7,2,99))]
for offset in (0,4,7,8,9):
    for status in ('available','unavailable'):
        rows=copy.deepcopy(values);rows[1]['status']=status
        for frame in ('main','inline'):
            job('x',[],environment='native',symbols=dict(scopes=scopes,variables=variables),values=rows,offset=offset,frame=frame)
for has_vector in (False,True):
    typ=dict(vectors=[dict(path='',members=['.x','.y'])]) if has_vector else {}
    symbols=dict(scopes=scopes[:1],variables=[dict(id='v',name='s',type=typ)])
    vals=[dict(variable_id='v',scope_id='main',name='s.'+c,type='uint',status='available',bits=i+1) for i,c in enumerate('xy')]
    for text in ('s','s.x+s.y','s.yx','s[0]','s.z','s.xyz','s.rg','all(s>0)'):
        job(text,[],environment='native',symbols=symbols,values=vals,offset=0,frame='main')
        partial=copy.deepcopy(vals);partial[1]['status']='unavailable'
        job(text,[],environment='native',symbols=symbols,values=partial,offset=0,frame='main')
for kind,bits in [('float',0x7fc12345),('float',0xff800000),('float',0x80000000),('double',0x7ff8000000001234),
                 ('double',0xfff0000000000000),('double',0x8000000000000000),('int',0x80000000),('uint64',0xffffffffffffffff)]:
    symbols=dict(scopes=scopes[:1],variables=[dict(id='v',name='x',type={})])
    vals=[dict(variable_id='v',scope_id='main',name='x',type=kind,status='available',bits=bits)]
    for text in ('x','x+1','x*0','x-x','x%2','2%x','min(x,2)','min(2,x)','bool(x)','int(x)','!x','abs(x)'):
        job(text,[],environment='native',symbols=symbols,values=vals,offset=0,frame='main')
for duplicate in (False,True):
    syms=dict(status='available',variables=[dict(id='a',sdbg_id=0,name='x',return_value=False,type={})],
              instruction_map={'8':dict(visible_variables=[0,1])})
    vals=[dict(variable_id='a',name='x',type='uint',status='available',bits=7)]
    if duplicate:
        syms['variables'].append(dict(id='b',sdbg_id=1,name='x',return_value=False,type={}))
        vals.append(dict(variable_id='b',name='x',type='uint',status='available',bits=9))
    for offset in (0,8):
        for text in ('x','x+1','x.xx','false&&x'):
            job(text,[],environment='sdbg',symbols=syms,values=vals,offset=offset)

native_records=0
if a.captures:
    for path in sorted(a.captures.glob('spdb-*-trace-native/checkpoint.json')):
        report=json.loads(path.read_text());raw=(path.parent/'shader.dxbc').read_bytes();symbols=decode(raw);stack=build(raw,symbols)
        data=(path.parent/'snapshots.bin').read_bytes();meta=report['register_capture'];catalog={r['token']:r['word_offset']*4 for r in report['catalog']}
        for row in headers(data,meta):
            # One complete invocation per captured stage/phase/driver, with all its records.
            if row['invocation']!=0:continue
            offset=catalog[row['token']];frames=at(stack,offset,row.get('call_depth',0))['frames']
            if not frames:continue
            vals=resolve(symbols,register_values(data,meta,row['record']),meta,row,offset);native_records+=1
            for frame in frames:
                owned=[v for v in vals if v['scope_id']==frame['id']]
                names=list(dict.fromkeys(v['name'] for v in owned))[:5]
                for name in names:
                    if not name or '<' in name:continue
                    for text in (name,'('+name+') == ('+name+')'):
                        job(text,[],environment='native',symbols=symbols,values=vals,offset=offset,frame=frame['id'])

manifest=a.out/'jobs.json';manifest.write_text(json.dumps(jobs),encoding='utf-8')
(a.out/'expected.json').write_text(json.dumps(expected),encoding='utf-8')
env=dict(os.environ);env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH']
subprocess.run([str(a.exe.resolve()),'--probe',str(manifest)],env=env,check=True,timeout=180)
actual=json.loads(Path(str(manifest)+'.results.json').read_text());assert len(actual)==len(expected)
checks=[]
for j,want,got in zip(jobs,expected,actual):
    passed=want['success']==got['success'] and (not want['success'] or want['value']==got['value'])
    checks.append(dict(name=j['name'],expression=j['expression'],passed=passed,success=want['success']))
    if not passed:print(json.dumps(dict(job=j['name'],expression=j['expression'],expected=want,actual=got)),flush=True)
result=dict(passed=all(c['passed'] for c in checks),count=len(checks),success=sum(c['success'] for c in checks),
            native_records=native_records,checks=checks,executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),
            sources={s:hashlib.sha256((a.reference/'standalone'/s).read_bytes()).hexdigest() for s in
                     ('debug_expression.py','native_debug_expression.py','native_sdbg_expression.py')})
(a.out/'validation.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in result.items() if k not in ('checks','sources')}),flush=True)
raise SystemExit(0 if result['passed'] else 1)
