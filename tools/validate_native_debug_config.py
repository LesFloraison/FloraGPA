"""Development-only native shader debugger configuration interoperability."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
for name in ('reference', 'exe', 'qt-bin', 'captures', 'out'):
    p.add_argument('--'+name, type=Path, required=True)
a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
import native_debug_config as config
from native_source_trace import NativeSourceTrace
from debug_expression import Expression
jobs,expected=[],[]
def job(name,op,oracle,**args):
    jobs.append(dict(name=name,op=op,**args))
    try:expected.append(dict(success=True,value=oracle()))
    except (ValueError,KeyError,TypeError,IndexError,UnicodeError,RecursionError) as exc:
        expected.append(dict(success=False,error=str(exc)))
def prepare(name,result,value):
    def oracle():return config.export(result,*config.prepare(result,[],None,value))
    job(name,'native-debug-config',oracle,result=result,config=value)
def identity(name,result):job(name,'native-debug-identity',lambda:config.identity(result),result=result)

seen=set();capture_count=0
for path in sorted(a.captures.glob('*-native/checkpoint.json')):
    result=json.loads(path.read_text());identity(path.parent.name,result);capture_count+=1
    if result['shader_sha256'] in seen:continue
    seen.add(result['shader_sha256'])
    source=NativeSourceTrace(result,[])
    locations=[r['source_location'] for r in result['catalog'] if r.get('source_location') and r['checkpoint_allowed']]
    for i,loc in enumerate(locations[:3]):source.set_rule(loc['file'],loc['line_start'],'  1u == 1u  ',('always','equal','multiple')[i],i+1)
    instructions={r['instruction'] for r in result['catalog'] if r['checkpoint_allowed']}
    value=config.export(result,source,instructions,[Expression('  1u + 2u  '),Expression('false && missing')])
    prepare(path.parent.name+'-roundtrip',result,value)
    for field in ('shader_sha256','catalog','source_lines','source_variables','source_stack'):
        changed=copy.deepcopy(result)
        if field=='shader_sha256':changed[field]='f'*64
        elif isinstance(changed[field],dict):changed[field]['identity_test']=1
        else:changed[field]=changed[field][:-1]
        prepare(path.parent.name+'-mismatch-'+field,changed,value)

result=dict(action='gs-checkpoint',shader_sha256='a'*64,source_lines=dict(files=[dict(name='源😀.hlsl')]),
            source_variables={},source_stack={},catalog=[dict(instruction=i,word_offset=2+i,checkpoint_allowed=True,
            source_location=dict(file=0,line_start=i+1,line_end=i+1)) for i in range(300)])
value=config.export(result,NativeSourceTrace(result,[]),set(),[])
prepare('empty',result,value)
for stage in ('gs','ds','hs'):
    changed=copy.deepcopy(result);changed['action']=stage+'-checkpoint'
    valid=config.export(changed,NativeSourceTrace(changed,[]),{0,3},[Expression('42')])
    prepare('stage-'+stage,changed,valid)
    if stage!='gs':prepare('wrong-stage-'+stage,changed,value)
for field in value:
    invalid=copy.deepcopy(value);del invalid[field];prepare('missing-'+field,result,invalid)
invalid=dict(value,extra=True);prepare('extra-field',result,invalid)
for v in (None,[],False,0,'config'):
    prepare('root-'+str(v),result,v)
for field in ('format','identity','source_breakpoints','instruction_breakpoints','watches'):
    for v in (None,False,0,1.0,'wrong',{}):
        prepare(field+'-'+str(v),result,dict(value,**{field:v}))
for count in (0,1,64,65):
    prepare('watch-count-'+str(count),result,dict(value,watches=[str(i) for i in range(count)]))
for watches in (['a','a'],['a',' a '],['x=1'],['missing'],['1<<32'],[''],[' '],[False],[1],['x'*2049]):
    prepare('watch-'+str(watches)[:50],result,dict(value,watches=watches))
for instructions in ([0],[0,1,0],[True],[False],[1.0],[-1],[None],['1'],[301],[2**64-1],list(range(4097))):
    prepare('instructions-'+str(instructions)[:55],result,dict(value,instruction_breakpoints=instructions))
for count in (1,256,257):
    prepare('source-count-'+str(count),result,dict(value,source_breakpoints=[dict(file=0,line=i+1) for i in range(count)]))
def point(name,point):prepare('point-'+name,result,dict(value,source_breakpoints=[point]))
for v in (None,False,[],{},dict(file=0),dict(line=1),dict(file=0,line=1,unknown=0)):
    point(str(v),v)
for field in ('file','line'):
    for v in (None,False,True,'1',1.0,-1,301,2**64-1):
        point(field+'-'+str(v),dict(dict(file=0,line=1),**{field:v}))
for text in ('','  ','\u001c','  1u == 1u ','missing','x=1',False,0,None):
    point('condition-'+str(text),dict(file=0,line=1,condition=text))
for mode in ('always','equal','at_least','multiple','invalid',False,None):
    for count in (1,0,0xffffffff,0x100000000,-1,True,1.0,'1',None):
        point('count-'+str(mode)+'-'+str(count),dict(file=0,line=1,hit_count=dict(mode=mode,count=count)))
for rule in ({},dict(mode='always'),dict(count=1),dict(mode='always',count=1,extra=0),[],None):
    point('bad-rule-'+str(rule),dict(file=0,line=1,hit_count=rule))
prepare('duplicate-source',result,dict(value,source_breakpoints=[dict(file=0,line=1),dict(file=0,line=1)]))
for field,values in [('action',[None,False,'debug-pixel','GS-checkpoint','']),('shader_sha256',[None,False,'A'*64,'a'*63,'g'*64,'a'*65])]:
    for v in values:identity('bad-identity-'+field+'-'+str(v),dict(result,**{field:v}))
identity('missing-mappings',dict(action='gs-checkpoint',shader_sha256='a'*64))
canonical=dict(result,source_variables={'😀':{'源':'\n\t\x00\\/\"\u2028'},'numbers':[0.,-0.,1e-5,1e-4,1e15,1e16,2**64-1]})
identity('canonical-unicode-numbers',canonical)
rng=random.Random(90437)
for i in range(500):
    number=struct.unpack('<d',rng.getrandbits(64).to_bytes(8,'little'))[0]
    if number!=number or abs(number)==float('inf'):continue
    identity('canonical-float-'+str(i),dict(action='gs-checkpoint',shader_sha256='a'*64,source_variables=dict(value=number)))

# Native output is read back and compared semantically; field order in pretty
# JSON is immaterial. Existing Python config bytes must import unchanged.
unicode_value=dict(value,watches=['1u'],label='源😀')
job('write-roundtrip','native-debug-config-write',lambda:unicode_value,output=str(a.out/'written.json'),config=unicode_value)
raw=json.dumps(unicode_value,ensure_ascii=False)
for encoding in ('utf-8','utf-8-sig','utf-16','utf-16-le','utf-16-be','utf-32','utf-32-le','utf-32-be'):
    path=a.out/(encoding+'.json');path.write_bytes(raw.encode(encoding))
    job('read-'+encoding,'native-debug-config-read',lambda path=path:config.read(path),input=str(path))
for name,data in [('empty',b''),('invalid',b'{bad}'),('bad-utf8',b'"\xff"'),('unfinished',b'{"a":'),
                  ('one-mib',b'"'+b'a'*(1024*1024-2)+b'"'),('too-large',b'"'+b'a'*(1024*1024-1)+b'"')]:
    path=a.out/(name+'.json');path.write_bytes(data)
    job('read-'+name,'native-debug-config-read',lambda path=path:config.read(path),input=str(path))
large='a'*(1024*1024)
job('write-too-large','native-debug-config-write',lambda:config.write(a.out/'python-too-large.json',large),
    output=str(a.out/'native-too-large.json'),config=large)
manifest=a.out/'jobs.json';manifest.write_text(json.dumps(jobs,ensure_ascii=False),encoding='utf-8')
(a.out/'expected.json').write_text(json.dumps(expected,ensure_ascii=False),encoding='utf-8')
env=dict(os.environ);env['PATH']=str(a.qt_bin)+os.pathsep+env.get('PATH','')
subprocess.run([str(a.exe.resolve()),'--probe',str(manifest)],env=env,check=True)
actual=json.loads(Path(str(manifest)+'.results.json').read_text());assert len(actual)==len(expected)
checks=[]
for j,w,g in zip(jobs,expected,actual):
    checks.append(dict(name=j['name'],passed=w['success']==g['success'] and (not w['success'] or w['value']==g['value'])))
report=dict(passed=all(c['passed'] for c in checks),count=len(checks),successes=sum(v['success'] for v in expected),
            capture_identities=capture_count,unique_shaders=len(seen),checks=checks,
            executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),
            reference_sha256=hashlib.sha256((a.reference/'standalone/native_debug_config.py').read_bytes()).hexdigest())
(a.out/'validation.json').write_text(json.dumps(report,indent=2))
print(json.dumps({k:v for k,v in report.items() if k!='checks'}),flush=True)
for c in checks:
    if not c['passed']:print(c,flush=True)
raise SystemExit(0 if report['passed'] else 1)
