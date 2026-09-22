"""Compare native pass/consumer state transitions and saved original observations."""
import argparse
import copy
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys


def bits(v):
    if isinstance(v,float):return {'$f':struct.pack('<d',v).hex()}
    if isinstance(v,(list,tuple)):return [bits(x) for x in v]
    if isinstance(v,dict):return {k:bits(x) for k,x in v.items()}
    return v


def payload(item):
    if item.get('null'):return None
    if 'times' in item:
        data=bytearray(8+52*len(item['times']));struct.pack_into('<II',data,0,item.get('header',0x18c00000),52*len(item['times']))
        for i,pair in enumerate(item['times']):struct.pack_into('<dd',data,28+i*52,*pair)
    else:
        data=bytearray(20);typ=item['type'];struct.pack_into('<I',data,0,item.get('header',0x14000000 if typ==0xa0 else 0x14600000))
        data[12:20]=bytes.fromhex(item['bits']) if 'bits' in item else struct.pack('<Q' if typ==0xa0 else '<d',item.get('value',0))
    return bytes(data).hex()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ['reference','exe','qt-bin','out']:p.add_argument('--'+key,type=Path,required=True)
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False);sys.path.insert(0,str(a.reference/'standalone'))
    from metric_consumer import MetricConsumer
    from metric_pass_controller import MetricPassController,ProbeFanout
    jobs=[];expected=[];labels=[];oracles=[];sources=[];rng=random.Random(88429)
    def consumer_state(c):return dict(handles=list(c.handles),counts=c.counts[:],rows=copy.deepcopy(c.rows),timings=[list(t) for t in c.timings])
    def feed(c,item):c.receive(item['handle'],item.get('type',0),bytes.fromhex(item['payload']) if item.get('payload') is not None else None,item.get('present',True))
    def reference(j):
        output=[]
        if j['kind']=='consumer':
            c=MetricConsumer(j['handles'],j.get('timing',0))
            for step in j['steps']:
                try:
                    if step['op']=='reset':c.reset(step['handles'])
                    else:
                        for item in step['items']:feed(c,item)
                    row=dict(result=None)
                except (ValueError,RuntimeError) as e:row=dict(error=str(e))
                row['state']=consumer_state(c);output.append(row)
            return output
        trace=[];behavior=j.get('behavior',{})
        def fail(name):
            if behavior.get('throw')==name:raise RuntimeError('injected '+name)
        class Lock:
            def __init__(self,name):self.name=name
            def set_priority(self,v):trace.append([self.name,'priority',v]);fail(self.name+'.priority')
            def acquire(self):trace.append([self.name,'acquire']);fail(self.name+'.acquire');return behavior.get('acquire',True)
            def release(self):trace.append([self.name,'release']);fail(self.name+'.release')
        class Backend:
            def begin_probe(self,h,k0,k1,tag,ctx):trace.append(['begin',h,k0,k1,tag,ctx]);fail('begin');return 9 if behavior.get('failed_probe',0)==h else 0
            def end_probe(self,h,key,tag,ctx):trace.append(['end',h,key,tag,ctx]);fail('end');return 9 if behavior.get('failed_probe',0)==h else 0
        class Publisher(ProbeFanout):
            def set_pool(self,size):
                if j.get('oracle_pool'):
                    trace.append(['lookup','methods\\QueryPoolSize'])
                    if j.get('pool_method',True):trace.append(['pool',0xbeef,0x18400800,size])
                else:trace.append(['pool',size])
                fail('pool')
            def subscribe(self,c,h):trace.append(['subscribe',c,h]);fail('subscribe');return 7 if behavior.get('failed_subscription',0)==h else 0
            def unsubscribe(self,c,h):trace.append(['unsubscribe',c,h]);fail('unsubscribe');return 17
            def configure(self,key,data):trace.append(['configure',key,data.hex()]);fail('configure');return behavior.get('configured',True)
            def flush(self,c):trace.append(['flush',c]);fail('flush')
        if j['kind']=='fanout':
            fanout=ProbeFanout(j['handles'],Backend())
            for step in j['steps']:
                trace.clear();behavior=step.get('behavior',behavior)
                try:row=dict(result=fanout.begin(0xfedcba9876543210,9,0x87654321,0x12345678) if step['op']=='begin' else fanout.end(0xfedcba9876543210,0x87654321,0x12345678))
                except (ValueError,RuntimeError) as e:row=dict(error=str(e))
                row['trace']=copy.deepcopy(trace);output.append(row)
            return output
        publisher=Publisher(j.get('probes',[9,0,2]),Backend())
        c=MetricPassController(j['handles'],j.get('requests',[]),j.get('passes',[]),publisher,Lock('primary'),Lock('secondary') if j.get('secondary',True) else None,**j.get('options',{}))
        c.current_pass=j.get('current',2**32-1);c.completed_probes=j.get('count',0)
        for step in j['steps']:
            trace.clear();behavior=step.get('behavior',behavior);row=dict(result=None)
            try:
                op=step['op']
                if op=='select':c.select(step['value'])
                elif op=='begin':c.begin(step['value'])
                elif op=='end':c.end()
                elif op=='flush':c.flush()
                elif op=='finish':c.finish(step.get('value',True))
                elif op=='seed_count':c.completed_probes=step['value']
                elif op=='key':c.probe_mode=step['value']
                elif op=='feed':feed(c.consumer,step['item'])
                elif op=='configure':row['result']=c.configure(step['value'],bytes.fromhex(step.get('configuration','030000000100000000000000')))
                elif op=='prepare':
                    def provider(key):trace.append(['configuration',key]);fail('configuration');return bytes.fromhex(step.get('configuration','030000000100000000000000'))
                    row['result']=c.prepare(c.requests if step.get('alias') else step['requests'],step['compatibility'],step['value'],provider)
                else:raise ValueError('Unknown action')
            except (ValueError,RuntimeError) as e:row=dict(error=str(e))
            row.update(trace=copy.deepcopy(trace),state=dict(handles=list(c.handles),requests=list(c.requests),passes=[list(x) for x in c.passes],current_pass=c.current_pass,completed_probes=c.completed_probes,reported_pass_count=c.reported_pass_count,probe_mode=c.probe_mode,consumer=consumer_state(c.consumer)))
            output.append(row)
        return output
    def add(label,j):
        jobs.append(copy.deepcopy(j));labels.append(label)
        try:expected.append(bits(reference(j)))
        except (ValueError,RuntimeError) as e:expected.append(dict(error=str(e)))
        return len(jobs)-1
    def load(name,file='validation.json'):
        path=a.reference/'output/metric-scheduling'/name/file;d=json.loads(path.read_text())
        assert d.get('passed',True)
        assert hashlib.sha256(Path(d['binary']).read_bytes()).hexdigest()==d['sha256']
        sources.append(dict(path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest()));return d
    for row in load('consumer-oracle-v2','oracle.json')['rows']:
        case=row['case'];steps=[dict(op='feed',items=[dict(handle=x['handle'],type=x['type'],present=x.get('present',True),payload=payload(x)) for x in batch]) for batch in case['batches']]
        steps.append(dict(op='reset',handles=case['handles']))
        i=add('saved GPA callback',dict(kind='consumer',handles=case['handles'],timing=case['timing'],steps=steps))
        for s,want in enumerate(row['snapshots']):oracles.append((i,s,'consumer',want))
        oracles.append((i,len(steps)-1,'consumer',row['reset']))
    for row in load('pass-lifetime-oracle-verified')['cases']:
        steps=[]
        for s in row['snapshots']:
            if s['operation']=='feed':steps.append(dict(op='feed',item=dict(handle=s['argument'],type=0xa3,payload=payload(dict(type=0xa3,value=17.25))))) if not row['empty'] else steps.append(dict(op='seed_count',value=0))
            else:steps.append(dict(op=s['operation'],value=s['argument']))
        j=dict(kind='controller',handles=[101,202,303],requests=[] if row['empty'] else [0,1,2],passes=[[0,2],[1]],probes=row['probe_handles'],oracle_pool=True,pool_method=row['pool_method'],behavior=dict(acquire=row['lock_acquire_result']),options=dict(vendor=row['vendor'],probe_mode=row['probe_mode'],pool_size=row['pool_size'],timing_handle=303,consumer_handle=0xabc),steps=steps)
        i=add('saved GPA pass lifetime',j)
        for s,want in enumerate(row['snapshots']):oracles.append((i,s,'lifetime',want))
    for row in load('pass-prepare-key-oracle')['cases']:
        steps=[]
        for snap in row['snapshots']:
            # Replay the recorded provider's return status, not inferred manager output.
            configured=not any(t[0]=='create' and t[-2]!=0 for t in snap['trace'])
            config=next((t[2] for t in snap['trace'] if t[0]=='configuration'),'')
            steps.append(dict(op='prepare',requests=snap['requested'],alias=snap['alias'],value=snap['input_mode'],compatibility=row['choices'],configuration=config,behavior=dict(configured=configured)))
        i=add('saved GPA prepare state',dict(kind='controller',handles=[101,202,303,404],current=77,count=123,options=dict(probe_mode=5),steps=steps))
        for s,want in enumerate(row['snapshots']):oracles.append((i,s,'prepare',want))
    for roster in [[0],[-1],[True],[2**64-1],[1,1],[]]:add('consumer roster',dict(kind='consumer',handles=roster,steps=[]))
    bad_payloads=[None,'','00','00000014','00006014','0000c018','0000c01834000000']
    for timing,data,typ in itertools.product([0,11],bad_payloads,[0,0xa0,0xa3,2**32+0xa3,2**64-1]):
        add('callback validation',dict(kind='consumer',handles=[11],timing=timing,steps=[dict(op='feed',items=[dict(handle=11,type=typ,payload=data)]),dict(op='reset',handles=[11])]))
    for value in [-1.,float('inf'),float('nan'),float(2**64),-0.,1.9]:
        add('timing prefix and bounds',dict(kind='consumer',handles=[11],timing=11,steps=[dict(op='feed',items=[dict(handle=11,type=0,payload=payload(dict(times=[[1.,7.],[value,3.]])))])]))
    add('absent bypass and invalid reset',dict(kind='consumer',handles=[11],steps=[dict(op='feed',items=[dict(handle=999,type=0,payload='',present=False)]),dict(op='reset',handles=[11,11])]))
    for handles in [[],[0],[9,0,2],[2,9,2],[True],[-1],[2**64-1]]:
        steps=[dict(op=op,behavior=dict(failed_probe=h)) for h in [0,2,9] for op in ['begin','end']]
        add('probe order and failure',dict(kind='fanout',handles=handles,steps=steps))
    base=dict(kind='controller',handles=[11,22,33],requests=[0,1,2],passes=[[0,2],[1]])
    sequence=[dict(op='select',value=0),dict(op='begin',value=0),dict(op='end'),dict(op='finish',value=False),dict(op='select',value=0),dict(op='select',value=1),dict(op='seed_count',value=2**32-1),dict(op='end'),dict(op='finish'),dict(op='flush')]
    for vendor,pool,timing,acquire in itertools.product([0x8086,0x10de],[0,1,256,257,2**32-1],[0,11,22],[False,True]):add('pass lifetime',dict(base,options=dict(vendor=vendor,pool_size=pool,timing_handle=timing),behavior=dict(acquire=acquire),steps=sequence))
    for fault in ['primary.priority','primary.acquire','secondary.priority','secondary.acquire','pool','subscribe','begin','end','flush','unsubscribe','primary.release','secondary.release']:
        add('injected '+fault,dict(base,options=dict(vendor=0x10de),steps=[dict(step,behavior=dict(throw=fault)) for step in sequence]+[dict(op='finish',behavior={})]))
    for bad in [True,-1,2**32]:
        for key in ['vendor','probe_mode','pool_size','consumer_handle']:add('invalid constructor '+key,dict(base,options={key:bad},steps=[]))
        for op in ['select','begin','configure','key','finish']:add('invalid action '+op,dict(base,steps=[dict(op=op,value=bad)]))
    add('non-Intel missing lock',dict(base,options=dict(vendor=0x10de),secondary=False,steps=[]))
    for _ in range(100):
        steps=[]
        for n in range(8):
            requests=[rng.randrange(3) for _ in range(rng.randrange(5))]
            choices=[[rng.randrange(3) for _ in range(rng.randrange(4))] for _ in range(3)]
            steps.append(dict(op='prepare',requests=requests,compatibility=choices,value=rng.choice([0,6,2**32-1]),behavior=rng.choice([{},dict(configured=False),dict(throw='configuration'),dict(throw='configure')])))
            steps.append(dict(op='select',value=rng.randrange(3),behavior={}))
        add('request replacement and configuration',dict(base,steps=steps))
    request=a.out/'requests.json';response=a.out/'native.json'
    request.write_text(json.dumps(jobs,allow_nan=False));(a.out/'expected.json').write_text(json.dumps(expected))
    windows=Path(os.environ['SystemRoot']);env={k:v for k,v in os.environ.items() if k.upper() in {'SYSTEMROOT','SYSTEMDRIVE','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA'}}
    env['PATH']=str(a.qt_bin.resolve())+os.pathsep+str(windows/'System32')+os.pathsep+str(windows)
    subprocess.run([str(a.exe.resolve()),'--probe',str(request.resolve()),str(response.resolve())],env=env,check=True,timeout=120)
    actual=json.loads(response.read_text());assert len(actual)==len(expected)
    failures=[dict(index=i,name=labels[i],expected=w,actual=g) for i,(g,w) in enumerate(zip(actual,expected)) if g!=w]
    for i,s,kind,w in oracles:
        g=actual[i][s];state=g['state']
        if kind=='consumer':g=dict(rows=[[v['$f'] for v in r] for r in state['rows']],timings=state['timings'])
        elif kind=='lifetime':g=dict(trace=g['trace'],current=state['current_pass'],count=state['completed_probes'],rows=state['consumer']['rows']);w=bits({k:w[k] for k in g})
        else:g=dict(output=g['result'],requests=state['requests'],passes=state['passes'],device_key=state['probe_mode'],count=state['reported_pass_count'],current=state['current_pass'],completed=state['completed_probes']);w={k:w[k] for k in g}
        if g!=w:failures.append(dict(index=i,step=s,name='original '+kind,expected=w,actual=g))
    report=dict(passed=not failures,cases=len(jobs),steps=sum(len(j['steps']) for j in jobs),original_observations=len(oracles),provenance=sources,failures=failures,exe_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),sources={name:hashlib.sha256((a.reference/'standalone'/name).read_bytes()).hexdigest() for name in ['metric_consumer.py','metric_pass_controller.py']})
    (a.out/'validation.json').write_text(json.dumps(report,indent=2))
    print(json.dumps({k:report[k] for k in ['passed','cases','steps','original_observations']},indent=2))
    if failures:print(str(failures[0])[:6000]);return 1
    return 0


if __name__=='__main__':sys.exit(main())
