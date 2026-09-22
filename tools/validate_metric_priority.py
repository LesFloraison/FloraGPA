"""Priority table byte parity, acquisition failure order and accepted sidecars."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys
from unittest.mock import patch


def read(path): return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['reference','exe','qt-bin','out']:p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
    sys.path.insert(0,str(a.reference/'standalone'))
    from metric_priority import PriorityTable, PriorityState, resource_name, EMPTY
    from metric_priority_win32 import default_path
    import md_acquisition_priority as acq
    from md_publisher_values import load_publisher_result
    rng=random.Random(77139);jobs=[];expected=[];labels=[];provenance=[]

    def table_probe(j):
        buffer=PriorityTable.initial()
        if 'corrupt' in j:buffer[j['corrupt']]^=1
        table=PriorityTable(buffer);states={};out=[]
        for s in j['steps']:
            try:
                op=s['op'];row=s.get('row',0);result=None;alive=lambda pid:pid in s.get('alive',[])
                if op=='allocate':result=table.allocate(s['name'])
                elif op=='name':result=table.name(row).decode('ascii')
                elif op=='get':result=table.get(row,s['field'])
                elif op=='put':table.put(row,s['field'],s['value'])
                elif op=='insert':table.insert(row,s['pid'],s['tid'],s['priority'])
                elif op=='update':table.update(row,s['pid'],s['tid'],s['priority'])
                elif op=='remove':table.remove(row,s['pid'],s['tid'])
                elif op=='maximum':result=table.maximum(row)
                elif op=='cleanup':table.cleanup(row,alive)
                elif op=='empty':result=table.empty_after_cleanup(row,alive)
                elif op=='state':states[s['id']]=PriorityState(table,row,s.get('global',0),s['pid'],s['tid'],s['priority'],global_strategy=s.get('global_strategy',False))
                else:
                    state=states[s['id']]
                    if op=='set':state.set_priority(s['value'])
                    elif op=='eligible':result=state.eligible()
                    elif op=='highest':result=state.highest()
                    elif op=='grant':result=state.can_grant()
                    elif op=='owned':result=state.owned()
                    elif op=='acquire':result=state.try_acquire(alive)
                    elif op=='release':state.release()
                    elif op=='unregister':state.unregister()
                    else:raise AssertionError(op)
                result=dict(result=result)
            except (ValueError,OverflowError) as e:result=dict(error=str(e))
            result.update(sha256=hashlib.sha256(buffer).hexdigest(),priorities={key:s.priority for key,s in states.items()});out.append(result)
        return out

    def acquisition_probe(j):
        trace=[];calls={}
        def hit(name):
            trace.append(name);calls[name]=calls.get(name,0)+1
            if j.get('failure')==name and calls[name]==j.get('failure_call',1):
                raise (TimeoutError if name=='acquire' else RuntimeError)('injected '+name)
        class Metrics:
            catalog={};handle=1
            def close(self):hit('metrics_close');self.handle=None
        class Lock:
            closed=False;depth=0;priority=EMPTY
            def __init__(self):self.priorities=[]
            def set_priority(self,value):hit('set7' if value==7 else 'withdraw');self.priority=value;self.priorities.append(value)
            def acquire(self):hit('acquire');self.depth=1;return True
            def release(self):hit('release');self.depth=0
            def close(self):hit('close');self.closed=True;self.depth=0
            def audit(self):return dict(closed=self.closed,depth=self.depth,priority=self.priority,priorities=self.priorities[:])
        def factory(catalog):hit('factory');return Lock()
        metrics=Metrics();obj=acq.AcquisitionPriority(metrics,a.out);error=None
        def save():hit('save');trace.append(copy.deepcopy(obj.report()))
        obj.save=save
        try:
            with patch.object(acq,'metric_device_mutex',factory),obj:
                if j.get('outer_failure'):raise RuntimeError('injected outer')
                for i in range(j.get('passes',2)):
                    with obj.replay(i,'A',i):
                        hit('work')
                        if j.get('nested'):
                            with obj.replay(9,'nested',0):pass
                        if j.get('close_active'):obj.close()
        except (ValueError,RuntimeError,TimeoutError) as e:error=dict(type=type(e).__name__,message=str(e))
        return dict(error=error,report=obj.report(),trace=trace,metrics_closed=metrics.handle is None)

    def reference(j):
        op=j['op']
        if op=='name':return resource_name(j['kind'],j['low'],j['high'],j['suffix'])
        if op=='table':return table_probe(j)
        if op=='acquisition':return acquisition_probe(j)
        if op=='default_path':return str(default_path())
        if op=='priority_result':return acq.validate_priority_result(j['folder'],j['profile'],required=j.get('required',False))
        if op=='publisher_result':return load_publisher_result(Path(j['folder']),j['profile'])
        raise AssertionError(op)

    def add(label,**j):
        jobs.append(copy.deepcopy(j));labels.append(label)
        try:expected.append(reference(j))
        except (ValueError,OverflowError,RuntimeError) as e:expected.append(dict(error=str(e)))
    add('Documents folder',op='default_path')
    for i in range(100):add('resource naming',op='name',kind=rng.choice([0,1,EMPTY]),low=rng.choice([0,1,rng.randrange(2**32)]),high=rng.randrange(2**32),suffix=rng.choice(['OA','UI','GLOBAL','Mixed_123','',chr(0)]))
    for field,value in [('kind',True),('low',-1),('high',2**32),('suffix','中文')]:
        j=dict(op='name',kind=1,low=1,high=0,suffix='OA');j[field]=value;add('invalid naming',**j)
    base=[dict(op='allocate',name=name) for name in ['GLOBAL','oa','UI']]
    for i in range(80):
        steps=copy.deepcopy(base)
        for k,row in enumerate([0,1,1,2]):
            pid=100+k;tid=200+k
            steps += [dict(op='insert',row=row,pid=pid,tid=tid,priority=EMPTY),dict(op='state',id=str(k),row=row,pid=pid,tid=tid,priority=EMPTY,global_strategy=row==0),dict(op='set',id=str(k),value=rng.choice([EMPTY,0,7,8,0x80000000,0x7fffffff]))]
        for _ in range(45):
            op=rng.choice(['set','eligible','highest','grant','owned','acquire','release','unregister','cleanup','empty','maximum'])
            step=dict(op=op,id=str(rng.randrange(4)),row=rng.randrange(3),value=rng.choice([EMPTY,0,1,7,8,0x80000000]),alive=rng.sample([100,101,102,103],rng.randrange(5)))
            steps.append(step)
        add('table transitions '+str(i),op='table',steps=steps)
    invalid=[dict(op='allocate',name=''),dict(op='allocate',name='a'*128),dict(op='allocate',name='a\0b'),dict(op='allocate',name='中文'),
        dict(op='name',row=16),dict(op='get',row=0,field=0),dict(op='put',row=0,field=0x80,value=True),
        dict(op='insert',row=1,pid=EMPTY,tid=1,priority=1),dict(op='update',row=1,pid=100,tid=200,priority=1)]
    add('table validation',op='table',steps=base+invalid)
    add('row capacity',op='table',steps=[dict(op='allocate',name='resource'+str(i)) for i in range(17)])
    steps=base+[dict(op='insert',row=1,pid=i,tid=1,priority=i) for i in range(1025)]
    steps += [dict(op='insert',row=1,pid=0,tid=1,priority=0),dict(op='remove',row=1,pid=2,tid=1),dict(op='insert',row=1,pid=2000,tid=2,priority=1)]
    add('entry capacity and reuse',op='table',steps=steps)
    for offset in [0,8]:add('corrupt header',op='table',steps=[],corrupt=offset)
    for failure in ['factory','save','set7','acquire','work','withdraw','release','metrics_close','close']:
        for count in range(1,7):add(f'acquisition failure {failure}/{count}',op='acquisition',failure=failure,failure_call=count)
    for j in [dict(passes=0),dict(passes=4),dict(nested=True),dict(close_active=True),dict(outer_failure=True),dict(outer_failure=True,failure='metrics_close'),dict(nested=True,failure='metrics_close')]:add('acquisition scope',op='acquisition',**j)

    # Each fixture is isolated; the validator never overwrites reference artifacts.
    def fixture(label,profile,audit=None,publisher=None,required=False):
        folder=a.out/('fixture-'+str(len(jobs)));folder.mkdir()
        if audit is not None:(folder/'priority-audit.json').write_text(json.dumps(audit),encoding='utf-8')
        if publisher is not None:(folder/'publisher-values.json').write_text(json.dumps(publisher),encoding='utf-8')
        add(label,op='publisher_result' if publisher is not None else 'priority_result',folder=str(folder.resolve()),profile=profile,required=required)
    profile=dict(arbitration='gpa_priority_v2',priority_audit='priority-audit.json',adapter_luid=[42,-1],validation=dict(passes=[dict(pass_index=0,set='A',sample_index=0)]))
    audit=dict(schema_version=1,policy='uniform_set_then_iteration',failure=None,passes=[dict(pass_index=0,set='A',sample_index=0,acquired=True,complete=True,failure=None,counters_closed_after_failure=False)],
        lock=dict(resource=resource_name(1,42,EMPTY,'OA'),closed=True,depth=0,priority=EMPTY,timeouts=0,cross_process=True,production_gpa_dependency=False,priorities=[7,EMPTY],attempts=1))
    fixture('accepted audit',profile,audit)
    fixture('legacy optional',{})
    fixture('legacy required',{},required=True)
    for field,value in [('arbitration','bad'),('priority_audit','../priority-audit.json')]:
        changed=copy.deepcopy(profile);changed[field]=value;fixture('invalid audit reference',changed,audit)
    for field,values in [('closed',[False,1]),('resource',['bad']),('priority',[7]),('depth',[1]),('timeouts',[1]),('attempts',[0,True,-1,2**64-1]),('priorities',[[],[7,7]]),('cross_process',[False,1]),('production_gpa_dependency',[True,0])]:
        for value in values:
            changed=copy.deepcopy(audit);changed['lock'][field]=value;fixture('audit lock '+field,profile,changed)
    for field,value in [('schema_version',2),('policy','bad'),('failure',{}),('passes',[])]:
        changed=copy.deepcopy(audit);changed[field]=value;fixture('audit '+field,profile,changed)
    for field,value in [('acquired',False),('complete',False),('failure',{}),('set','bad'),('complete',1)]:
        changed=copy.deepcopy(audit);changed['passes'][0][field]=value;fixture('pass '+field,profile,changed)
    root=a.reference/'output/metric-scheduling'
    for pointer in (root/'publisher-statistics-desktop').glob('*profile-path.txt'):
        path=Path(pointer.read_text().strip());raw=read(path);pub=read(path.parent/'publisher-values.json')
        provenance += [dict(path=str(p),sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in [path,path.parent/'publisher-values.json']]
        fixture('saved publisher '+pointer.name,raw,publisher=pub)
        for field,value in [('publisher_values','../publisher-values.json'),('arbitration','bad')]:
            changed=copy.deepcopy(raw);changed[field]=value;fixture('publisher reference '+field,changed,publisher=pub)
        for mutation in [lambda p:p['records'].pop(),lambda p:p['records'].reverse(),lambda p:p['records'][0].update(raw_sha256='bad'),
            lambda p:p['records'][0].update(available=not p['records'][0]['available']),lambda p:p['records'][0]['values'].pop(),
            lambda p:p['records'][0]['information'].append({}),lambda p:p['records'][0]['values'][0].update(name='bad'),
            lambda p:p['records'][0]['values'][0].update(unit='bad'),lambda p:p['records'][0]['values'][0].update(value=True),
            lambda p:p['records'][0]['values'][0].update(value=-1.),lambda p:p['records'][0]['values'][0].update(typed_hex='00'),
            lambda p:p['records'][0]['values'][0].update(typed_hex='gg')]:
            changed=copy.deepcopy(pub);mutation(changed);fixture('publisher corruption '+pointer.name,raw,publisher=changed)
        # Typed bytes are authoritative; JSON cannot hide signed zero or a rounded scalar.
        for typed,value in [('02000000000000000000000000000000',0.),('02000000000000000000008000000000',-0.),
                            ('02000000000000000000008000000000',0.),('02000000000000000000807f00000000',0.),
                            ('02000000000000000000c07f00000000',0.)]:
            changed=copy.deepcopy(pub);changed['records'][0]['values'][0].update(typed_hex=typed,value=value)
            fixture('publisher typed scalar '+pointer.name,raw,publisher=changed)
        changed_raw=copy.deepcopy(raw);changed_raw['mode']='recovered_metric_iterations';changed_raw['arbitration']='ignored-by-recorded-mode'
        fixture('recorded mode audit policy',changed_raw,publisher=pub)
        changed_raw=copy.deepcopy(raw);changed_raw.update(arbitration='gpa_priority_v2',priority_audit='priority-audit.json')
        valid=copy.deepcopy(audit)
        low,high=raw['adapter_luid'];valid['lock']['resource']=resource_name(1,low,high & EMPTY,'OA')
        valid['passes']=[dict(pass_index=p['pass_index'],set=p['set'],sample_index=p['sample_index'],acquired=True,complete=True,failure=None,counters_closed_after_failure=False) for p in raw['validation']['passes']]
        valid['lock']['priorities']=[v for _ in valid['passes'] for v in [7,EMPTY]];valid['lock']['attempts']=len(valid['passes'])
        fixture('publisher audited acceptance',changed_raw,valid,pub)
        valid['lock']['closed']=False;fixture('publisher rejects incomplete ownership',changed_raw,valid,pub)

    request,response=a.out/'requests.json',a.out/'native.json';request.write_text(json.dumps(jobs),encoding='utf-8')
    (a.out/'expected.json').write_text(json.dumps(expected,indent=2),encoding='utf-8')
    env={k:v for k,v in os.environ.items() if k.upper() in {'SYSTEMROOT','SYSTEMDRIVE','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA'}}
    windows=Path(os.environ['SystemRoot']);env['PATH']=str(a.qt_bin.resolve())+os.pathsep+str(windows/'System32')+os.pathsep+str(windows)
    subprocess.run([str(a.exe.resolve()),'--probe',str(request.resolve()),str(response.resolve())],check=True,env=env,timeout=180)
    actual=read(response);assert len(actual)==len(expected)
    differences=[dict(index=i,name=labels[i],expected=e,actual=g) for i,(e,g) in enumerate(zip(expected,actual)) if json.dumps(e,sort_keys=True)!=json.dumps(g,sort_keys=True)]
    report=dict(passed=not differences,cases=len(jobs),table_steps=sum(len(j.get('steps',[])) for j in jobs),provenance=provenance,
        sources={n:hashlib.sha256((a.reference/'standalone'/n).read_bytes()).hexdigest() for n in ['metric_priority.py','metric_priority_win32.py','md_acquisition_priority.py','md_publisher_values.py']},
        exe_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),differences=differences)
    (a.out/'validation.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(dict(passed=report['passed'],cases=len(jobs),table_steps=report['table_steps'],failures=[dict(index=d['index'],name=d['name']) for d in differences]),indent=2))
    if differences:print(str(differences[0])[:5000]);return 1
    return 0


if __name__=='__main__':sys.exit(main())
