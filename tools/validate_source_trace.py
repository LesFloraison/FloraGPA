"""Development-only native source navigation, watches and breakpoint parity."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
for name in ('reference','exe','qt-bin','out','captures'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=False)
sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
from native_source_trace import NativeSourceTrace
from native_source_variables import resolve
from debug_expression import Expression
from gs_checkpoint import headers,register_values

jobs=[];expected=[];total_queries=0;actual_records=0
def scenario(name,result,rows,queries,values=None):
    global total_queries
    total_queries+=len(queries)
    jobs.append(dict(name=name,op='source-trace',result=result,rows=rows,queries=queries,**(dict(values=values) if values is not None else {})))
    loads=0
    def loader(i):
        nonlocal loads
        loads+=1;return values[i]
    nav=NativeSourceTrace(result,rows,loader if values is not None else None);answers=[]
    for q in queries:
        try:
            action=q['action'];i=q.get('index',0);v=None
            if action=='location':v=nav.location(i)
            elif action=='stack':v=nav.stack(i)
            elif action=='path':v=nav.path(i)
            elif action=='same':v=nav.same(i,q['other'])
            elif action=='reentry':v=nav.reentry(i)
            elif action=='next':v=nav.next(i,q['direction'])
            elif action=='function':v=nav.function_step(i,q.get('out',False))
            elif action=='seek':v=nav.seek(i)
            elif action=='entry':v=nav.breakpoint_entry(i)
            elif action=='line-entry':v=nav.line_entry(i,(q['file'],q['line']))
            elif action=='count':v=nav.encounter_count(i,(q['file'],q['line']))
            elif action=='toggle':nav.toggle(q['file'],q['line'])
            elif action=='rule':nav.set_rule(q['file'],q['line'],q.get('text',''),q.get('mode','always'),q.get('count',1))
            elif action=='export':v=nav.export_breakpoints()
            elif action=='copy':
                cloned=NativeSourceTrace(result,rows,loader if values is not None else None);cloned.copy_rules(nav);v=cloned.export_breakpoints()
            elif action=='watch':
                value=nav.environment(i,q.get('frame')).expression(Expression(q['text']));v=dict(type=value.kind,text=value.text())
            else:raise ValueError(action)
            answers.append(dict(success=True,value=v,loads=loads))
        except (ValueError,KeyError,IndexError,TypeError) as exc:answers.append(dict(success=False,error=str(exc),loads=loads))
    expected.append(json.loads(json.dumps(answers)))

def navigation(rows):
    result=[]
    for i in range(len(rows)):
        result.extend([dict(action=k,index=i) for k in ('location','stack','path','reentry','entry','seek')])
        result.extend([dict(action='next',index=i,direction=d) for d in (-1,1)])
        result.extend([dict(action='function',index=i,out=out) for out in (False,True)])
    return result

result=dict(trace=True,shader_sha256='shader',source_lines=dict(files=[dict(text='one\ntwo\nthree\nfour')]),
    catalog=[dict(instruction=i,word_offset=2+i*2,checkpoint_allowed=True,source_location=dict(file=0,line_start=i+1,line_end=i+1)) for i in range(4)],
    source_stack=dict(status='available',frames=[dict(id='main',name='main',kind='function',parent=None,ranges=[dict(start=8,end=40)]),
                                               dict(id='leaf',name='leaf',kind='inline',parent='main',ranges=[dict(start=16,end=32)])]),
    source_variables=dict(scopes=[dict(id='main',parent=None,kind='function'),dict(id='leaf',parent='main',kind='inline')],
        variables=[dict(id='x',name='x',type={}),dict(id='arg',name='x',type={})]))
instructions=[0,1,1,2,3,0,1,2,3,0,1]
rows=[dict(record=i,invocation=0 if i<9 else 1,hit=i if i<9 else i-9,instruction=ins) for i,ins in enumerate(instructions)]
values=[[dict(variable_id=id,scope_id=scope,name='x',type='uint',status='available',bits=i) for id,scope in [('x','main'),('arg','leaf')]] for i in range(len(rows))]
scenario('plain',result,rows,navigation(rows),values)
for mode in ('always','equal','at_least','multiple'):
    for text in ('','x >= 6','x > 100','missing','false && missing','uint2(1,2)'):
        queries=[dict(action='rule',file=0,line=2,text=text,mode=mode,count=2),dict(action='export'),dict(action='copy')]
        queries+=navigation(rows)+[dict(action='count',index=i,file=0,line=2) for i in range(len(rows))]
        queries += [dict(action='toggle',file=0,line=2),dict(action='export')]
        scenario('rules-'+mode+'-'+text,result,rows,queries,values)
queries=[dict(action='rule',file=0,line=2,text='missing'),dict(action='toggle',file=0,line=3)]
multi=copy.deepcopy(result);multi['catalog'][1]['source_location']['line_end']=3
scenario('unconditional-overrides-failing-condition',multi,rows,queries+navigation(rows),values)
for name,mutate in [('missing-stack',lambda r:r.pop('source_stack')),
                    ('single-point',lambda r:r.update(trace=False,checkpoint_instruction=dict(instruction=0))),
                    ('missing-locations',lambda r:[e.pop('source_location') for e in r['catalog']]),
                    ('unmapped-function',lambda r:r['source_stack']['frames'][0].update(ranges=[dict(start=8,end=16)]))]:
    r=copy.deepcopy(result);mutate(r)
    scenario(name,r,rows,navigation(rows)+[dict(action='watch',index=0,text='x')],values)
scenario('truncated-tail',result,rows[:3],navigation(rows[:3]),values[:3])
scenario('missing-origin',result,rows[2:9],[dict(action='rule',file=0,line=2,mode='equal',count=1)]+navigation(rows[2:9]),values[2:9])
phase=copy.deepcopy(rows)
for i,row in enumerate(phase):row['hs_phase']=0 if i<4 else 1
scenario('phase-boundary',result,phase,navigation(phase),values)
calls=copy.deepcopy(rows)
for i,row in enumerate(calls):row['call_depth']=1 if 2<=i<5 else 0
scenario('original-call-depth',result,calls,navigation(calls),values)
queries=[]
for i in (0,0,1,1,0):
    queries.extend([dict(action='watch',index=i,text='x'),dict(action='watch',index=i,text='x',frame='main'),dict(action='watch',index=i,text='x',frame='foreign')])
scenario('value-loader-cache',result,rows,queries,values)
scenario('no-value-loader',result,rows,queries)
invalid=[dict(action='rule',file=0,line=2,text='x=1'),dict(action='rule',file=0,line=2,mode='bad'),
         dict(action='rule',file=0,line=2,count=0),dict(action='rule',file=0,line=2,count=4294967296),
         dict(action='rule',file=-1,line=2),dict(action='toggle',file=0,line=99),
         dict(action='next',index=0,direction=0)]
scenario('invalid-rules',result,rows,[dict(action='rule',file=0,line=2,text='x > 1')]+[q for item in invalid for q in (item,dict(action='export'))],values)
many=copy.deepcopy(result)
many['catalog']=[dict(instruction=i,word_offset=i+2,checkpoint_allowed=True,source_location=dict(file=0,line_start=i+1,line_end=i+1)) for i in range(300)]
scenario('breakpoint-limit',many,rows,[dict(action='toggle',file=0,line=i+1) for i in range(257)]+[
    dict(action='rule',file=0,line=1,text='true'),dict(action='rule',file=0,line=258),dict(action='export'),dict(action='copy')])
legacy=copy.deepcopy(result);legacy['source_variables']=dict(format='SDBG assignments',status='available',shader_sha256='shader',
    variables=[dict(id='x',sdbg_id=0,name='x',return_value=False,type={})],
    instruction_map={str((2+i*2)*4):dict(visible_variables=[0]) for i in range(4)})
for label,edit in [('valid',lambda r:None),('wrong-shader',lambda r:r.update(shader_sha256='wrong')),
                   ('single-point',lambda r:r.update(trace=False))]:
    r=copy.deepcopy(legacy);edit(r);scenario('sdbg-'+label,r,rows,queries,values)

for path in sorted(a.captures.glob('spdb-*-trace-native/checkpoint.json')):
    report=json.loads(path.read_text());meta=report['register_capture'];data=(path.parent/'snapshots.bin').read_bytes()
    catalog={e['token']:e for e in report['catalog']};rows=headers(data,meta)
    # Include a full invocation plus the first record of its successor to verify stopping.
    boundary=next((i for i,r in enumerate(rows) if r['invocation']!=rows[0]['invocation']),len(rows))
    rows=rows[:boundary+1];actual_records+=len(rows)
    values=[]
    for row in rows:
        entry=catalog[row['token']];row['instruction']=entry['instruction']
        values.append(resolve(report['source_variables'],register_values(data,meta,row['record']),meta,row,entry['word_offset']*4))
    scenario(path.parent.name,report,rows,navigation(rows),values)
    mapped=[e['source_location'] for e in report['catalog'] if e.get('source_location') and e['checkpoint_allowed']]
    if not mapped:continue
    point=mapped[len(mapped)//2];queries=[]
    for mode in ('always','equal','at_least','multiple'):
        queries.append(dict(action='rule',file=point['file'],line=point['line_start'],mode=mode,count=2,text='true'))
        queries.extend(dict(action='entry',index=i) for i in range(len(rows)))
        queries.extend(dict(action='function',index=i,out=out) for i in range(len(rows)) for out in (False,True))
        queries.append(dict(action='seek',index=0))
    scenario(path.parent.name+'-rules',report,rows,queries,values)

manifest=a.out/'jobs.json';manifest.write_text(json.dumps(jobs),encoding='utf-8')
(a.out/'expected.json').write_text(json.dumps(expected),encoding='utf-8')
env=dict(os.environ);env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH']
subprocess.run([str(a.exe.resolve()),'--probe',str(manifest)],env=env,check=True,timeout=180)
actual=json.loads(Path(str(manifest)+'.results.json').read_text());assert len(actual)==len(expected)
checks=[];success=0;rejected=0
for j,want,got in zip(jobs,expected,actual):
    differences=[]
    if not got['success']:differences.append(dict(error=got))
    else:
        if len(want)!=len(got['value']):differences.append(dict(count=len(got['value'])))
        for index,(w,g) in enumerate(zip(want,got['value'])):
            success+=w['success'];rejected+=not w['success']
            if w['success']!=g['success'] or w['loads']!=g['loads'] or (w['success'] and w['value']!=g['value']):
                differences.append(dict(index=index,query=j['queries'][index],expected=w,actual=g))
    checks.append(dict(name=j['name'],passed=not differences,differences=differences))
    if differences:print(json.dumps(checks[-1]),flush=True)
result=dict(passed=all(c['passed'] for c in checks),scenarios=len(checks),queries=total_queries,
            success=success,rejections=rejected,native_records=actual_records,checks=checks,
            executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),
            source_sha256=hashlib.sha256((a.reference/'standalone/native_source_trace.py').read_bytes()).hexdigest())
(a.out/'validation.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in result.items() if k!='checks'}),flush=True)
raise SystemExit(0 if result['passed'] else 1)
