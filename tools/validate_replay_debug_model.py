"""Development-only recorded shader navigation, variable and configuration parity."""
import argparse
import ast
import copy
import json
import os
from pathlib import Path
import random
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
for key in ('reference','exe','qt-bin','captures','out'):
    p.add_argument('--'+key,type=Path,required=True)
a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=False)
sys.path.insert(0,str(a.reference/'standalone'))
from debug_state import TraceState,variable_rows
from source_debug import SourceTrace
from source_variables import SourceVariables
from debug_expression import Expression
import debug_config

jobs=[];expected=[]


def oracle(result,ops):
    state=TraceState(result);source=SourceTrace(result);values=SourceVariables(result);watches=[];outputs=[]
    def watch_results():
        rows=[]
        for e in watches:
            row=dict(expression=e.text)
            try:row.update(status='available',**e.evaluate(values.resolve(state)).snapshot())
            except Exception as exc:row.update(status='unavailable',error=str(exc))
            rows.append(row)
        return rows
    for op in ops:
        name=op['op'];pos=op.get('position',state.position)
        try:
            if name=='move':
                state.move(pos);v=dict(position=state.position,variables=copy.deepcopy(state.variables),values=values.resolve(state),location=source.location(pos),stack=source.stack(pos))
            elif name=='rows':v=[row for var in list(state.variables.values())+result['trace'].get('constantBlocks',[]) for row in variable_rows(var,op.get('mode','float'))]
            elif name=='location':v=source.location(pos)
            elif name=='stack':v=source.stack(pos)
            elif name=='info':v=source.info.get(op['instruction'])
            elif name=='next':v=source.next_source(pos,op['direction'])
            elif name=='function':v=source.step_function(pos,op['mode'])
            elif name=='toggle':source.toggle(op['file'],op['line']);v=source.export_breakpoints()
            elif name=='condition':source.set_condition(op['file'],op['line'],op['text']);v=source.export_breakpoints()
            elif name=='hit-count':source.set_hit_count(op['file'],op['line'],op['mode'],op['count']);v=source.export_breakpoints()
            elif name=='entry':v=source.line_entry(pos,(op['file'],op['line']))
            elif name=='count':v=source.encounter_count(pos,(op['file'],op['line']))
            elif name=='breakpoint':v=source.breakpoint_entry(pos)
            elif name=='seek':v=source.continue_to_breakpoint(pos)
            elif name=='watch':
                e=Expression(op['text'])
                if e.text not in [x.text for x in watches]:
                    if len(watches)>=64:raise ValueError('Watch limit')
                    watches.append(e)
                v=watch_results()
            elif name=='unwatch':
                if not 0<=op['index']<len(watches):raise ValueError('Watch selection')
                del watches[op['index']];v=watch_results()
            elif name=='watches':v=watch_results()
            elif name=='eval':
                probe=TraceState(result);probe.move(pos);value=Expression(op['text']).evaluate(values.resolve(probe));v=dict(**value.snapshot(),text=value.text())
            elif name=='config':v=debug_config.export(result,source,watches)
            elif name=='import':source,watches=debug_config.prepare(result,op['config']);v=debug_config.export(result,source,watches)
            elif name=='report':
                v=dict(result,source_breakpoints=source.export_breakpoints(),source_variable_snapshot=dict(step=state.position,variables=values.resolve(state)),callstack_snapshot=dict(step=state.position,functions=list(source.stack(state.position))),watch_snapshot=dict(step=state.position,expressions=watch_results()),debug_config=debug_config.export(result,source,watches))
            else:raise ValueError(name)
            outputs.append(dict(success=True,value=copy.deepcopy(v),position=state.position))
        except Exception as exc:outputs.append(dict(success=False,error=str(exc),position=state.position))
    return outputs


def add(name,result,ops):
    jobs.append(dict(name=name,result=result,ops=ops))
    try:expected.append(dict(success=True,actions=oracle(result,ops)))
    except Exception as exc:expected.append(dict(success=False,error=str(exc)))


real=[]
for path in sorted(a.captures.glob('*-native/result.json')):
    result=json.loads(path.read_text('utf-8'))
    if not result.get('ok') or 'steps' not in result:continue
    real.append(result);source=SourceTrace(result);ops=[];count=len(result['steps'])
    order=list(range(count))+list(reversed(range(count)))+random.Random(71).sample(list(range(count)),count)
    for i in order:ops.append(dict(op='move',position=i))
    for i in range(count):
        for direction in (-1,1):ops.append(dict(op='next',position=i,direction=direction))
        for mode in ('over','out'):ops.append(dict(op='function',position=i,mode=mode))
    for mode in ('float','int','uint','hex'):ops.append(dict(op='rows',mode=mode))
    ops += [dict(op='watch',text='2u + 3u'),dict(op='watch',text='missing_name + 1'),dict(op='watches'),dict(op='config'),dict(op='report')]
    add(path.parent.name,result,ops)
    points=sorted({(loc[0],loc[1]) for loc in source.locations.values()})
    if not points:continue
    if path.parent.name.startswith('loop-'):
        # Conditions must inspect their own step state without moving the visible
        # cursor. Exercise real loop locals, unavailable scopes and reverse seeks.
        operations=[dict(op='move',position=count-1)]
        for expression in ('value', 'i', 'value > 49.0', 'i == 1u', 'i < 2u && value > 48.0'):
            for i in range(count):operations.append(dict(op='eval',position=i,text=expression))
        for file,line in points:
            operations.append(dict(op='condition',file=file,line=line,text='value > 49.0'))
            operations += [dict(op='seek',position=i) for i in range(-1,count)]
            operations.append(dict(op='toggle',file=file,line=line))
        operations += [dict(op='watch',text='value'),dict(op='watch',text='i')]
        for i in reversed(range(count)):
            operations += [dict(op='move',position=i),dict(op='watches')]
        add(path.parent.name+'-real-locals',result,operations)
    for file,line in points[:4]:
        operations=[dict(op='move',position=count-1)]
        for mode in ('always','equal','at_least','multiple'):
            operations += [dict(op='condition',file=file,line=line,text='true'),dict(op='hit-count',file=file,line=line,mode=mode,count=2)]
            for i in reversed(range(count)):
                operations += [dict(op='entry',file=file,line=line,position=i),dict(op='count',file=file,line=line,position=i),dict(op='breakpoint',position=i)]
        operations += [dict(op='condition',file=file,line=line,text='false && missing_name > 0'),dict(op='seek',position=-1),dict(op='condition',file=file,line=line,text='missing_name / 0'),dict(op='seek',position=-1),dict(op='move',position=0),dict(op='config')]
        add(path.parent.name+'-rule-'+str(file)+'-'+str(line),result,operations)
    file,line=points[0];config=debug_config.export(result,SourceTrace(result),[Expression('1u')]);config['breakpoints']=[dict(file=file,line=line,condition='true',hit_count=dict(mode='multiple',count=2))]
    bad=[]
    for key,value in [('identity',{}),('format','wrong'),('watches',['1',' 1 ']),('breakpoints',[dict(file=file,line=9999999)]),('breakpoints',[dict(file=True,line=line)]),('breakpoints',[dict(file=file,line=line,hit_count=dict(mode='multiple',count=0))])]:
        modified=copy.deepcopy(config);modified[key]=value;bad.append(modified)
    ops=[dict(op='move',position=count-1),dict(op='import',config=config)]
    for c in bad:ops += [dict(op='import',config=c),dict(op='config')]
    ops.append(dict(op='report'));add(path.parent.name+'-config',result,ops)


def variable(name,kind=4,values=None,members=None):
    return dict(name=name,type=kind,rows=1,columns=1,value={k:list(values or [7]) for k in ('u32v','s32v','f32v','f64v','f16v','u64v','s64v','u16v','s16v','u8v','s8v')},members=members or [])


def mapping(name,register,kind=4,category=6,**extra):
    return dict(dict(name=name,type=kind,rows=1,columns=1,offset=0,variables=[dict(type=category,name=register,component=0)]),**extra)


synthetic=dict(action='debug-pixel',disassembly='test asm',source_debug=dict(files=[dict(filename='测试.hlsl',contents='one\r\ntwo\nthree\u2028four')]),trace=dict(inputs=[variable('in',values=[2])],constantBlocks=[variable('cb',members=[variable('[0]',values=[41]),variable('[1]',values=[43])])],instInfo=[dict(instruction=0,lineInfo=dict(fileIndex=0,lineStart=1,lineEnd=1),sourceVars=[mapping('a','r')]),dict(instruction=5,lineInfo=dict(fileIndex=0,lineStart=2,lineEnd=2),sourceVars=[mapping('b','r')])],sourceVars=[mapping('array','cb',kind=255,category=2),mapping('unknown','missing'),mapping('undefined','r',undefinedValue=True)]),steps=[])
empty=variable('');empty['rows']=empty['columns']=0
for i,(instruction,frames) in enumerate([(0,['main']),(1,['main','child']),(5,['main','child']),(5,['main']),(0,['main']),(5,['main'])]):
    before=empty if i==0 else variable('r',values=[i-1]);after=variable('r',values=[i])
    synthetic['steps'].append(dict(nextInstruction=instruction,changes=[dict(before=before,after=after)],callstack=frames))
add('synthetic-nesting',synthetic,[dict(op='move',position=i) for i in [0,5,2,4,0]]+[dict(op='function',position=i,mode=m) for i in range(6) for m in ['over','out']]+[dict(op='watch',text='a'),dict(op='rows',mode='hex')])
for name,patch in [('duplicate-path',lambda r:r['trace']['constantBlocks'][0]['members'].append(variable('[0]'))),('undefined',lambda r:r['trace']['instInfo'][0]['sourceVars'].append(mapping('undef','r',undefinedValue=True))),('partial',lambda r:r['trace']['instInfo'][0]['sourceVars'].append(mapping('partial','r',columns=4))),('too-many',lambda r:r['trace']['instInfo'][0]['sourceVars'].append(mapping('over','r',variables=[dict(type=6,name='r',component=0)]*2))),('missing-stack',lambda r:r['steps'][2].update(callstack=[])),('discontinuous',lambda r:r['steps'][2].update(callstack=['foreign'])),('bad-file',lambda r:r['trace']['instInfo'][0]['lineInfo'].update(fileIndex=6)),('duplicate-info',lambda r:r['trace']['instInfo'].append(copy.deepcopy(r['trace']['instInfo'][0]))),('negative-info',lambda r:r['trace']['instInfo'][0].update(instruction=-1))]:
    changed=copy.deepcopy(synthetic);patch(changed)
    add(name,changed,[dict(op='move',position=i) for i in range(6)]+[dict(op='function',position=0,mode='over')])

source=a.out/'jobs.json';source.write_text(json.dumps(jobs,ensure_ascii=False),encoding='utf-8')
env=dict(os.environ);env['PATH']=str(a.qt_bin.resolve())+';'+os.environ['WINDIR']+'/System32;'+os.environ['WINDIR']
run=subprocess.run([str(a.exe.resolve()),'--probe',str(source)],env=env,capture_output=True,timeout=240)
(a.out/'native.log').write_bytes(run.stdout+run.stderr)
assert run.returncode==0,run.stderr.decode(errors='replace')
actual=json.loads(Path(str(source)+'.results.json').read_text('utf-8'));checks=[];diagnostics=0
assert len(actual)==len(expected)==len(jobs), 'Native probe omitted a scenario'


def comparable(value):
    global diagnostics
    value=copy.deepcopy(value)
    def walk(v):
        global diagnostics
        if isinstance(v,dict):
            if v.get('status')=='unavailable' and 'error' in v:v['error']='<diagnostic>'
            if 'issues' in v:
                prefix='Unavailable type or component: '
                for i,message in enumerate(v['issues']):
                    if message.startswith(prefix):
                        raw=message[len(prefix):]
                        try:ref=json.loads(raw)
                        except ValueError:ref=ast.literal_eval(raw)
                        v['issues'][i]=prefix+json.dumps(ref,sort_keys=True)
            for x in v.values():walk(x)
        elif isinstance(v,list):
            for x in v:walk(x)
    walk(value)
    return value


for job,got,want in zip(jobs,actual,expected):
    ok=got['success']==want['success'];differences=[]
    if ok and got['success']:
        ok=len(got['actions'])==len(want['actions'])
        for i,(g,w) in enumerate(zip(got['actions'],want['actions'])):
            same=g['success']==w['success'] and g['position']==w['position']
            if same and g['success']:
                gv,wv=comparable(g['value']),comparable(w['value'])
                if job['ops'][i]['op']=='rows':gv=sorted(gv);wv=sorted(wv)
                same=gv==json.loads(json.dumps(wv))
            if not same:differences.append(dict(index=i,op=job['ops'][i]))
        ok=ok and not differences
    checks.append(dict(name=job['name'],passed=ok,operations=len(job['ops']),differences=differences[:12]))
report=dict(passed=all(c['passed'] for c in checks),traces=len(real),scenarios=len(checks),operations=sum(c['operations'] for c in checks),checks=checks)
(a.out/'validation.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
(a.out/'expected.json').write_text(json.dumps(expected,ensure_ascii=False),encoding='utf-8')
print(json.dumps({k:v for k,v in report.items() if k!='checks'},indent=2))
for c in checks:
    if not c['passed']:print(c['name'],c['differences'])
assert report['passed']
