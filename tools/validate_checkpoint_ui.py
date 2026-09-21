"""Compare actual Qt-selected values/frames/watches and ZIP exports to Python."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import zipfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--artifacts',type=Path,required=True)
a=p.parse_args();sys.path[:0]=[str(a.reference/'standalone'),str(a.reference/'tools')]
from gs_checkpoint import register_values,headers
from native_source_variables import resolve
from native_sdbg_variables import TraceValues
from native_source_trace import NativeSourceTrace
from native_frame_sources import location,locals_for
from debug_expression import Expression
import native_debug_config as config
from gs_checkpoint_ui import CheckpointWindow
checks=[];sessions={}
def check(name,passed):
    checks.append(dict(name=name,passed=bool(passed)))
    if not passed:print(checks[-1],flush=True)
def session(path):
    if path in sessions:return sessions[path]
    root=Path(path);r=json.loads((root/'checkpoint.json').read_text());raw=(root/'snapshots.bin').read_bytes()
    rows=r['hits_preview'];meta=r['register_capture'];entries={e['instruction']:e for e in r['catalog'] if e['instruction'] is not None}
    read=lambda i:register_values(raw,meta,rows[i]['record'])
    if r['source_variables'].get('format')=='SDBG assignments' and r.get('trace'):
        history=TraceValues(r,rows,read);values=history.at
    else:values=lambda i:resolve(r['source_variables'],read(i),meta,rows[i],entries[rows[i]['instruction'] if 'instruction' in rows[i] else r['checkpoint_instruction']['instruction']]['word_offset']*4)
    trace=NativeSourceTrace(r,rows,values);sessions[path]=(r,rows,entries,values,trace)
    return sessions[path]
for i,item in enumerate(json.loads((a.artifacts/'selected-values.json').read_text())):
    r,rows,entries,values,trace=session(item['capture']);index=item['index'];wanted=values(index)
    actual=item['variables']
    check(f'{i}-variables',(actual is None and not wanted) or actual==dict(record=rows[index]['record'],values=wanted))
    frame=item['frame'];frame_id=None if frame is None else frame['frame_id']
    if frame is not None:
        expected=dict(record=rows[index]['record'],frame_id=frame_id,
                      location=location(r['source_stack'],frame_id,entries[rows[index]['instruction']]['word_offset']*4),
                      variables=locals_for(r['source_variables'],wanted,frame_id))
        check(f'{i}-frame',frame==expected)
    environment=None
    try:environment=trace.environment(index,frame_id)
    except (ValueError,KeyError):pass
    watch=item['watches'];check(f'{i}-watch-context',watch['record']==rows[index]['record'] and watch['frame_id']==frame_id)
    for j,value in enumerate(watch['values']):
        expected=dict(status='unavailable',type='')
        if environment is not None:
            try:
                v=environment.expression(Expression(value['expression']));expected.update(status='available',type=v.kind,text=v.text())
            except ValueError:pass
        check(f'{i}-watch-{j}',all(value[k]==v for k,v in expected.items()) and bool(value['text']))
        if r['source_variables'].get('format')=='SDBG assignments':check(f'{i}-basis-{j}',value.get('basis')=='sdbg_assignment_history')
navigation=json.loads((a.artifacts/'navigation.json').read_text())
for i,item in enumerate(navigation):
    r,rows,entries,values,_=session(item['capture'])
    trace=NativeSourceTrace(r,rows,values)
    for rule in item['rules']:
        count=rule.get('hit_count',{})
        trace.set_rule(rule['file'],rule['line'],rule.get('condition',''),count.get('mode','always'),count.get('count',1))
    start=item['start'];action=item['action'];target=[start];rejected=False
    try:
        if item['mode']==1 and action!='RunTo':
            if action in ('Previous','Step'):target[0]=trace.next(start,-1 if action=='Previous' else 1)
            elif action in ('Over','Out'):target[0]=trace.function_step(start,action=='Out')
            else:target[0]=trace.seek(start)
        else:
            # Invoke the original Python controller methods without creating a
            # Tk window; position/go are the only UI boundaries for these methods.
            controller=CheckpointWindow.__new__(CheckpointWindow)
            controller.rows=rows;controller.position=lambda:start
            controller.go=lambda index:target.__setitem__(0,index)
            if action in ('Previous','Step'):controller.move(-1 if action=='Previous' else 1)
            elif action in ('Over','Out'):controller.seek_depth(action=='Out')
            else:controller.seek({item['instruction']})
    except ValueError:rejected=True
    check(f'navigation-{i}',rejected==item['rejected'] and target[0]==item['target'])
archives=[]
for path in sorted(a.artifacts.glob('*.zip')):
    with zipfile.ZipFile(path) as z:
        check(path.name+'-crc',z.testzip() is None)
        if path.name=='archive-transaction.zip':
            check('zip-unicode-binary',z.read('源.txt')==b'abc\0def' and z.read('empty')==b'')
            continue
        names={'checkpoint.json','shader.asm','shader.dxbc','snapshots.bin','hits.csv','registers.csv','result.json','loaded_modules.json'}
        r=json.loads(z.read('checkpoint.json'));meta=r['register_capture']
        if 'input_selector' in meta:
            names.add('input-selector.json');check(path.name+'-selector',json.loads(z.read('input-selector.json'))==meta['input_selector'])
        check(path.name+'-members',set(z.namelist())==names)
        check(path.name+'-shader',hashlib.sha256(z.read('shader.dxbc')).hexdigest()==r['shader_sha256'])
        check(path.name+'-records',len(headers(z.read('snapshots.bin'),meta))==r['record_count'])
        prepared=config.prepare(r,r['hits_preview'],None,r['native_debug_config'])
        check(path.name+'-settings',config.export(r,*prepared)==r['native_debug_config'])
        check(path.name+'-runtime',json.loads(z.read('result.json'))['loaded_modules']==json.loads(z.read('loaded_modules.json')))
        archives.append(dict(name=path.name,sha256=hashlib.sha256(path.read_bytes()).hexdigest(),records=r['record_count']))
report=dict(passed=bool(checks) and all(c['passed'] for c in checks),checks=checks,count=len(checks),
            captured_sessions=len(sessions),navigation_actions=len(navigation),archives=archives)
(a.artifacts/'validation.json').write_text(json.dumps(report,indent=2))
print(json.dumps({k:v for k,v in report.items() if k!='checks'}),flush=True)
raise SystemExit(0 if report['passed'] else 1)
