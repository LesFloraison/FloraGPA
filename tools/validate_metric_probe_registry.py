"""Native probe registry, packed device configuration and DX11 result parity."""
import argparse
import copy
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import subprocess
import sys
from validate_metric_pass_controller import bits


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ['reference','exe','qt-bin','out']:p.add_argument('--'+key,type=Path,required=True)
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False);sys.path.insert(0,str(a.reference/'standalone'))
    from metric_probe_registry import ProbeRegistry
    from metric_probe_config import dx11_probe_configuration,dx11_configuration_for_key
    from dx11_metric_result import receive_metric_result
    jobs=[];labels=[];expected=[];oracles=[];sources=[];rng=random.Random(774004)
    def reference(j):
        if j['kind']=='configuration':return dx11_probe_configuration(j['pointer']).hex()
        if j['kind']=='device_key':return dx11_configuration_for_key({key:(pointer,contexts) for key,pointer,contexts in j['devices']},j['key']).hex()
        if j['kind']=='result':return receive_metric_result(j['ids'],j['tokens'],j['rows'],j.get('timings',[]),j.get('initial'))
        trace=[];behavior=j.get('behavior',{});output=[]
        def fail(name):
            if behavior.get('throw')==name:raise RuntimeError('injected '+name)
        class Backend:
            def __init__(self):self.attempts={};self.serial=0;self.owners={};self.release_failure=True
            def lookup(self,parent,path):
                trace.append(['lookup',parent,path]);fail('lookup')
                if parent==0:return (7 if behavior.get('lookup_policy')=='first_fail' else 0),behavior.get('parent',555)
                return (8 if behavior.get('lookup_policy')=='second_fail_with_value' else 0),behavior.get('kind',int(path.rsplit('/',1)[-1]))
            def create_probe(self,kind,data):
                self.attempts[kind]=self.attempts.get(kind,0)+1;self.serial+=1;status=0;value=0x1000+self.serial
                if kind==behavior.get('first_kind',9) and self.attempts[kind]==1:
                    policy=behavior.get('creation_policy')
                    if policy in ('zero_success','zero_failure'):value=0
                    if policy=='zero_failure':status=9
                    if policy=='value_failure':status=-1
                value=behavior.get('created_value',value)
                if value:self.owners[value]=kind
                trace.append(['create',kind,data.hex(),status,value]);fail('create');return status,value
            def destroy_probe(self,value):
                status=9 if self.release_failure and self.owners[value]==behavior.get('last_kind',9) else 0
                if status:self.release_failure=False
                trace.append(['destroy',value,status]);fail('destroy');return status
            def begin_probe(self,h,x,y,tag,ctx):trace.append(['begin',h,x,y,tag,ctx]);fail('begin');return 9 if h==behavior.get('failed_probe',0) else 0
            def end_probe(self,h,key,tag,ctx):trace.append(['end',h,key,tag,ctx]);fail('end');return 9 if h==behavior.get('failed_probe',0) else 0
        registry=ProbeRegistry(Backend())
        for step in j['steps']:
            trace.clear();behavior=step.get('behavior',behavior);row=dict(result=None)
            try:
                op=step['op']
                if op=='register':row['result']=registry.register(step['path'])
                elif op=='configure':row['result']=registry.configure(step['group'],bytes.fromhex(step.get('configuration','')))
                elif op=='begin':row['result']=registry.begin(100,200,6,900)
                elif op=='end':row['result']=registry.end(200,6,900)
                elif op=='release':registry.release()
                else:raise ValueError('Unknown registry action')
            except (ValueError,RuntimeError) as e:row=dict(error=str(e))
            row.update(state=dict(groups={str(k):v[:] for k,v in registry.groups.items()},probes={str(k):v for k,v in registry.probes.items()},handles=list(registry.handles)),trace=copy.deepcopy(trace));output.append(row)
        return output
    def add(label,j):
        jobs.append(copy.deepcopy(j));labels.append(label)
        try:expected.append(bits(reference(j)))
        except (ValueError,RuntimeError) as e:expected.append(dict(error=str(e)))
        return len(jobs)-1
    def load(name):
        path=a.reference/'output/metric-scheduling'/name/'validation.json';d=json.loads(path.read_text());assert d['passed']
        assert hashlib.sha256(Path(d['binary']).read_bytes()).hexdigest()==d['sha256']
        sources.append(dict(path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest()));return d
    for case in load('probe-registry-oracle')['cases']:
        pattern=case['pattern'];steps=[]
        for i,s in enumerate(case['snapshots']):
            step=dict(op=s['operation'])
            if step['op']=='register':step['path']=f"probe_types/{s['argument']}"
            if step['op']=='configure':step.update(group=s['argument'],configuration=(bytes([i%256])*12).hex())
            steps.append(step)
        j=dict(kind='registry',behavior=dict(creation_policy=case['creation_policy'],lookup_policy=case['lookup_policy'],first_kind=next((x for x in pattern if x),9),last_kind=max((x for x in pattern if x),default=9)),steps=steps)
        i=add('saved GPA registry',j)
        for step,want in enumerate(case['snapshots']):oracles.append((i,step,'registry',want))
    config=load('pass-prepare-key-oracle')
    for row in config['configuration_cases']:
        i=add('saved packed device bytes',dict(kind='configuration',pointer=row['device']));oracles.append((i,None,'direct',row['configuration']))
    for row in config['lookup_cases']:
        devices=[[int(key),*value] for key,value in row['devices'].items()]
        i=add('saved device/context lookup',dict(kind='device_key',devices=devices,key=row['key']))
        oracles.append((i,None,'direct',row['configuration'] if row['configuration'] is not None else dict(error='Invalid reference to device')))
    for name in ['dx11-result-oracle-final','dx11-result-real-oracle']:
        for row in load(name)['cases']:
            initial={k:v for k,v in row['initial'].items() if k!='status'}
            i=add('saved DX11 callback result',dict(kind='result',ids=row['ids'],tokens=row['tokens'],rows=row['rows'],timings=row['timings'],initial=initial));oracles.append((i,None,'direct',bits(row['output'])))
    for pointer in [True,-1,2**64,0,1,2**53+1,2**63,2**64-1,1.,None]:add('pointer domain',dict(kind='configuration',pointer=pointer))
    for key in [True,-1,2**32,0,1,7,9,2**32-1]:
        for devices in [[],[[9,999,[7]],[1,111,[7,0,2**32-1]]],[[9,999,[1]],[1,0,[]]],[[0,-1,[]],[1,111,[]]],[[1,111,[True,9.]], [2**32-1,777,[9]]]]:add('device lookup precedence',dict(kind='device_key',key=key,devices=devices))
    for key in [True,-1,2**32]:add('invalid device table key',dict(kind='device_key',key=0,devices=[[key,1,[]]]))
    for _ in range(100):
        keys=rng.sample(range(25),rng.randrange(6));devices=[[key,rng.getrandbits(64),[rng.randrange(25) for _ in range(4)]] for key in keys]
        add('random unsigned lookup',dict(kind='device_key',key=rng.randrange(25),devices=devices))
    seed=dict(metrics=[dict(metric=9,values=[-0.],aux=[17])],timings=[[2**64-1,9]],extra='preserved')
    for ids,rows,timings,tokens in itertools.product([[],[1],[1,1],[0,2**32-1]], [[],[[]],[[1.]],[[1.,2.],[3.,4.]],[[True]],[[1.],[2.,3.]]], [[],[[0,2**64-1]],[[True,0]],[[0]]], [[],[0],[1,2,3]]):add('result guards and matrix',dict(kind='result',ids=ids,rows=rows,timings=timings,tokens=tokens,initial=seed))
    for value in [-0.,float('nan'),float('inf'),float('-inf'),2**64-1,-2**63,'1',None,True]:add('callback scalar policy',dict(kind='result',ids=[1],rows=[[value]],tokens=[0]))
    for ids in [[True],[-1],[2**32]]:add('callback ID bounds',dict(kind='result',ids=ids,rows=[],tokens=[]))
    base=[dict(op='register',path='probe_types/9'),dict(op='register',path='probe_types/2'),dict(op='register',path='probe_types/9'),dict(op='configure',group=0,configuration='0011'),dict(op='configure',group=0),dict(op='begin'),dict(op='end'),dict(op='release'),dict(op='begin'),dict(op='release')]
    for policy,lookup in itertools.product(['normal','zero_success','zero_failure','value_failure'],['normal','first_fail','second_fail_with_value']):add('cache and partial release',dict(kind='registry',behavior=dict(creation_policy=policy,lookup_policy=lookup),steps=base))
    for field in ['parent','kind','created_value']:
        for value in [True,-1,2**64-1,0]:add('backend handle validation',dict(kind='registry',behavior={field:value},steps=base))
    for fault in ['lookup','create','begin','end','destroy']:add('backend exception '+fault,dict(kind='registry',steps=[dict(s,behavior=dict(throw=fault)) for s in base]+[dict(op='release',behavior={})]))
    for path in ['',None,True,'probe\0type']:add('invalid registration path',dict(kind='registry',steps=[dict(op='register',path=path)]))
    for group in [True,-1,2**64-1]:add('group handle validation',dict(kind='registry',steps=[dict(op='configure',group=group)]))
    for _ in range(70):
        steps=[]
        for n in range(20):
            op=rng.choice(['register','configure','begin','end','release']);step=dict(op=op)
            if op=='register':step['path']=f'probe_types/{rng.choice([0,2,9,19,2**64-1])}'
            if op=='configure':step.update(group=rng.choice([0,0,1]),configuration=rng.randbytes(n).hex())
            steps.append(step)
        add('random registry lifecycle',dict(kind='registry',steps=steps))
    request=a.out/'requests.json';response=a.out/'native.json';request.write_text(json.dumps(bits(jobs),allow_nan=False));(a.out/'expected.json').write_text(json.dumps(expected))
    windows=Path(os.environ['SystemRoot']);env={k:v for k,v in os.environ.items() if k.upper() in {'SYSTEMROOT','SYSTEMDRIVE','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA'}};env['PATH']=str(a.qt_bin.resolve())+os.pathsep+str(windows/'System32')+os.pathsep+str(windows)
    subprocess.run([str(a.exe.resolve()),'--probe',str(request.resolve()),str(response.resolve())],env=env,check=True,timeout=120)
    actual=json.loads(response.read_text());assert len(actual)==len(expected)
    failures=[dict(index=i,name=labels[i],expected=w,actual=g) for i,(g,w) in enumerate(zip(actual,expected)) if json.dumps(g,sort_keys=True)!=json.dumps(w,sort_keys=True)]
    for i,step,mode,w in oracles:
        g=actual[i]
        if mode=='registry':g=dict(result=g[step]['result'],trace=g[step]['trace'],groups=g[step]['state']['groups'],probes=g[step]['state']['probes']);w={k:w[k] for k in g}
        if g!=w:failures.append(dict(index=i,step=step,name='original '+mode,actual=g,expected=w))
    report=dict(passed=not failures,cases=len(jobs),registry_actions=sum(len(j.get('steps',[])) for j in jobs),original_observations=len(oracles),provenance=sources,failures=failures,exe_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),sources={name:hashlib.sha256((a.reference/'standalone'/name).read_bytes()).hexdigest() for name in ['metric_probe_registry.py','metric_probe_config.py','dx11_metric_result.py']})
    (a.out/'validation.json').write_text(json.dumps(report,indent=2));print(json.dumps({k:report[k] for k in ['passed','cases','registry_actions','original_observations']},indent=2))
    if failures:print(str(failures[0])[:6000]);return 1
    return 0


if __name__=='__main__':sys.exit(main())
