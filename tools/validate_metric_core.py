"""Exact native/Python clock, calculated-report and query lifecycle comparisons."""
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--qt-bin', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--isolated-env', action='store_true')
    args = parser.parse_args(); args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.reference/'standalone'))
    from metric_clock import MetricClock, MetricsDiscoveryClockSource, ticks_to_nanoseconds
    from metric_report_values import typed_double, gpu_time_microseconds, MetricReportValues
    from metric_report_postprocess import timestamp_integer, uint_f32, postprocess, BusyState
    from metric_query_batch import QueryBatch
    from metric_query_drain import PendingPool, DeferredQueries, QueryDrain
    bits = lambda x: struct.pack('<d', x).hex()

    class Provider:
        def __init__(self, job):
            self.job=job; self.trace=[]; self.samples=iter(job['samples']); self.maximums=iter(job.get('maximums', [])); self.replace_offset=job.get('replace', False)
        def maximum(self):
            self.trace.append(['maximum'])
            return next(self.maximums) if 'maximums' in self.job else self.job['period']
        def sample(self, force):
            try: pair=next(self.samples)
            except StopIteration: raise RuntimeError('Clock fixture exhausted') from None
            self.trace.append(['sample',force,pair]); return pair

    def clock_state(clock): return [clock.reference,clock.offset,clock.previous_offset]

    class Sink:
        def __init__(self,cfg,trace): self.cfg=cfg; self.trace=trace; self.category=cfg.get('category',0)
        def fail(self,method):
            if self.cfg.get('throw')==method: raise RuntimeError('sink '+method)
        def set_key(self,key): self.trace.append([self.cfg['id'],'key',key]); self.fail('key')
        def complete(self,key,tag): self.trace.append([self.cfg['id'],'complete',key,tag]); self.fail('complete')
        def flush(self): self.trace.append([self.cfg['id'],'flush']); self.fail('flush')

    class Query:
        def __init__(self,cfg,trace): self.cfg=cfg; self.trace=trace; self.category=cfg.get('category',0); self.attempts=0
        def fail(self,method):
            if self.cfg.get('throw')==method: raise RuntimeError('query '+method)
        def begin(self,context): self.trace.append([self.cfg['id'],'begin']); self.fail('begin')
        def end(self,context): self.trace.append([self.cfg['id'],'end']); self.fail('end')
        def ready(self,flush):
            self.trace.append([self.cfg['id'],'ready',flush]); self.fail('ready')
            values=self.cfg.get('ready',[True]); value=values[min(self.attempts,len(values)-1)]; self.attempts+=1; return value
        def valid(self): self.trace.append([self.cfg['id'],'valid']); self.fail('valid'); return self.cfg.get('valid',True)
        def write_metric(self,sink): self.trace.append([self.cfg['id'],'write',sink.cfg['id']]); self.fail('write'); return self.cfg.get('write',True)

    def batch(cfg,trace):
        value=QueryBatch([None if q is None else Query(q,trace) for q in cfg['queries']],cfg.get('key0',0),cfg.get('key1',0),cfg.get('tag',0)); value.state=cfg.get('state',0); return value
    def slots(values): return {str(k):[v.key0 if v is not None else None for v in values] for k,values in values.items()}

    def reference(job):
        op=job['op']
        if op=='ticks': return ticks_to_nanoseconds(job['ticks'],job['frequency'])
        if op=='duration': return gpu_time_microseconds(job['value'])
        if op=='uint_float': return bits(uint_f32(job['value']))
        if op=='typed': return bits(typed_double(bytes.fromhex(job['record'])))
        if op=='timestamp': return timestamp_integer(bytes.fromhex(job['record']))
        if op=='source':
            pairs=iter(job['pairs']); trace=[]; rows=[]
            def read():
                pair=next(pairs);trace.append(pair);return pair
            source=MetricsDiscoveryClockSource(job['period'],read)
            for force in job['forces']:
                value=source.sample(force); rows.append(dict(sample=list(value),success=source.last_success,trace=trace[:]));trace.clear()
            return rows
        if op=='clock':
            provider=Provider(job);clock=MetricClock(provider);rows=[dict(**{'return':None},state=clock_state(clock),trace=provider.trace[:])];provider.trace.clear()
            for step in job['steps']:
                row={}
                try:
                    if step['op']=='cross': value=clock.crossed(step['earlier'],step['later'])
                    elif step['op']=='convert': value=clock.convert(step['value'])
                    else: value=clock.update(step.get('force',False))
                    row['return']=value
                except (ValueError,RuntimeError) as e:row['error']=str(e)
                row.update(state=clock_state(clock),trace=provider.trace[:]);provider.trace.clear();rows.append(row)
            return rows
        if op=='report':
            value=MetricReportValues(None if job['records'] is None else [bytes.fromhex(r) for r in job['records']],job.get('key',0));trace=[]
            class ValueSink:
                def requested_ids(self):trace.append(['requested']);return job['ids']
                def write_value(self,index,number):trace.append(['write',index,bits(number)])
                def set_key(self,key):trace.append(['key',key])
            reads=[value.read(index) for index in job['read']];written=value.write_metric(ValueSink())
            return dict(reads=[bits(v) if v is not None else None for v in reads],written=written,trace=trace)
        if op in ('postprocess','postprocess_batches'):
            provider=Provider(job);clock=MetricClock(provider);busy=BusyState(job.get('busy',0));results=[]
            for batch_ in [job['reports']] if op=='postprocess' else job['batches']:
                result={}
                try:
                    reports,keys=postprocess([[bytes.fromhex(v) for v in row] for row in batch_],job['metrics'],job['information'],clock,busy,job.get('normalize',True))
                    result.update(reports=[[v.hex() for v in row] for row in reports],keys=keys)
                except (ValueError,RuntimeError) as e:result['error']=str(e)
                result.update(state=clock_state(clock),trace=provider.trace[:],busy=busy.previous_key);results.append(result);provider.trace.clear()
            return results[0] if op=='postprocess' else results
        if op=='batch':
            trace=[];rows=[];value=batch(job,trace);metrics=[None if m is None else Sink(m,trace) for m in job['metrics']]
            for step in job['steps']:
                row={}
                try:
                    if step['op'] in ('begin','end'): result=getattr(value,step['op'])(None)
                    elif step['op']=='poll':result=value.poll(step.get('wait',False))
                    else:result=value.dispatch(metrics,step.get('wait',False))
                    row['return']=result
                except RuntimeError as e:row['error']=str(e)
                except AttributeError:row['error']='Missing query object'
                row.update(state=value.state,trace=trace[:]);trace.clear();rows.append(row)
            return rows
        if op=='pool':
            pending={int(k):[None if v is None else QueryBatch([],key0=v) for v in values] for k,values in job['slots'].items()}
            pool=PendingPool(pending,job.get('failures',0),job.get('failure_limit',0));rows=[];attempts={};trace=[]
            def accept(item):
                key=item.key0;trace.append(key);values=job['accept'][str(key)];index=attempts.get(key,0);attempts[key]=index+1;value=values[min(index,len(values)-1)]
                if isinstance(value,str):raise RuntimeError(value)
                return value
            for step in job['steps']:
                row={}
                try:row['return']=[v.key0 for v in pool.drain(step['slot'],step['limit'],accept)]
                except RuntimeError as e:row['error']=str(e)
                row.update(slots=slots(pool.slots),failures=pool.failures,trace=trace[:]);trace.clear();rows.append(row)
            return rows
        if op=='drain':
            trace=[];rows=[];batches={cfg['id']:batch(dict(cfg,key0=cfg['id'],state=cfg.get('state',2)),trace) for cfg in job['batches']}
            roster=lambda ids:[None if i is None else batches[i] for i in ids]
            pool=PendingPool({int(k):roster(ids) for k,ids in job['slots'].items()},job.get('failures',0),job.get('failure_limit',0));recycled={}
            deferred={int(k):DeferredQueries(roster(cfg['batches']),cfg.get('dirty',False)) for k,cfg in job['deferred'].items()}
            metrics=[None if m is None else Sink(m,trace) for m in job['metrics']]
            class Clock:
                def update(self,force):
                    trace.append(['clock',force])
                    if job.get('clock_error'):raise RuntimeError('clock error')
            drain=QueryDrain(pool if job.get('pending',True) else None,recycled if job.get('recycled',True) else None,metrics,Clock() if job.get('clock',True) else None,job.get('slot_count',0),job.get('limit',0),job.get('limited',False),deferred)
            for step in job['steps']:
                row={}
                try:row['return']=deferred[step['id']].arm() if step['op']=='arm' else drain.drain(step.get('wait',False))
                except RuntimeError as e:row['error']=str(e)
                row.update(trace=trace[:],slots=slots(pool.slots),recycled=slots(recycled),failures=pool.failures,states={str(i):b.state for i,b in batches.items()},dirty={str(i):d.dirty for i,d in deferred.items()});trace.clear();rows.append(row)
            return rows
        raise ValueError(op)

    rng=random.Random(0x464c4f5241);jobs=[]
    def add(op,**values):jobs.append(dict(op=op,**values))
    integers=[0,1,999,1000,1001,2**24-1,2**24+1,2**32-1,2**32,2**53-1,2**53+1,2**63-1,2**63,2**64-1]+[rng.getrandbits(64) for _ in range(1000)]
    for value in integers:
        add('duration',value=value);add('uint_float',value=value)
        add('ticks',ticks=value,frequency=rng.choice([1,1000,10**9,2**32,2**63,2**64-1,rng.randrange(1,2**64)]))
    add('ticks',ticks=0,frequency=0)
    for shift in range(25,41):
        for high in (2**23,2**23+1,2**24-1):
            midpoint=(high<<shift)+(1<<(shift-1))
            for delta in (-2049,-1025,-1024,-1,0,1,1023,1024,2049):
                if 0<=midpoint+delta<2**64:add('uint_float',value=midpoint+delta)
    def typed(kind,payload):return (struct.pack('<II',kind,0x1234abcd)+payload).hex()
    records_=[]
    for kind in (0,1,2,3,4,0xffffffff):
        for value in integers[:70]:
            record=typed(kind,struct.pack('<Q',value));records_.append(record);add('typed',record=record)
            if kind!=2:add('timestamp',record=record)
    for value in (-0.0,0.0,-0.5,-0.50001,0.49,0.5,0.75,1.,2**24-1,2**24,2**32,2**63,2**64,float('inf'),float('-inf'),float('nan')):
        record=typed(2,struct.pack('<fI',value,0xdeadbeef));add('typed',record=record);add('timestamp',record=record)
    for count in (0,1,8,15,17,32):add('typed',record=bytes(count).hex())
    for data in (None,[],records_[:6]):
        for ids in ([],[0],[0,1,2],[0,7,1],[5,0,5],[0xffffffff]):
            for key in (0,1,2**64-1):add('report',records=data,key=key,ids=ids,read=[0,1,5,6,0xffffffff])
    for period in (1000,2**32,2**56):
        for replace in (False,True):
            for initial in (0,100,-100,-2**63,2**63-1):
                samples=[[initial,period*96//100],[min(initial+period,2**63-1),period//100],[max(initial-1,-2**63),period*2//100],[0,period*3//100]]
                high,low=period*95//100,period*5//100
                steps=[dict(op='cross',earlier=a,later=b) for a,b in ((high,low-1),(high+1,low),(high+1,low-1),(period-1,0),(period//2,0),(0,period-1),(1,1))]
                steps += [dict(op='convert',value=x) for x in (period*97//100,period//100,period*98//100,period*2//100)]
                steps += [dict(op='update',force=False),dict(op='update',force=True)]
                add('clock',period=period,replace=replace,samples=samples,steps=steps)
    for period in (0,-1,2**63//95+1):add('clock',period=period,samples=[[100,960]],steps=[dict(op='cross',earlier=960,later=0)])
    add('clock',period=1000,maximums=[1000,2000],samples=[[0,0]],steps=[dict(op='cross',earlier=960,later=60)])
    for failures in range(4):
        pairs=[[1,0,0]]*failures+[[0,2**64-9,12]]*5
        add('source',period=1000,pairs=pairs,forces=[False,True])
    for period in (0,-1,2**63//95+1):add('source',period=period,pairs=[],forces=[])
    metrics=[dict(name='GpuTime',metric_type=0),dict(name='GpuBusy',metric_type=6),dict(name='Other',metric_type=7)]
    info=[dict(name='QueryBeginTime',information_type=3),dict(name='QueryEndTime',information_type=3)]
    u=lambda n:typed(1,struct.pack('<Q',n)); f=lambda n:typed(2,struct.pack('<fI',n,0xcafebabe))
    for index in range(700):
        rows=[];initial=rng.choice([0,100,-100]);period=1000000
        for offset in range(rng.randrange(1,5)):
            ns=rng.choice(integers);start=rng.choice([0,1,40000,50000,949999,950000,950001,960000,999999,2**64-1]);end=(start+123)&(2**64-1)
            rows.append([u(ns),f(23.5),u(rng.getrandbits(64)),u(start),u(end)])
        add('postprocess',metrics=metrics,information=info,reports=rows,period=period,samples=[[initial,960000]]+[[1000000+i*period,1000] for i in range(30)],busy=rng.choice([0,1,100,960000,2**64-1]),normalize=index%9!=0)
    base=dict(metrics=metrics,information=info,reports=[[u(1001),f(37),u(9),u(0),u(100)]],period=1000000,samples=[[100,0]],busy=15)
    for edit in ('no_time','no_begin','duplicate','wrong_time','wrong_busy','truncated','zero_gap','zero_duration','negative_time'):
        case=copy.deepcopy(base)
        if edit=='no_time':case['metrics'][0]['name']='OtherTime'
        elif edit=='no_begin':case['information'][0]['name']='OtherBegin'
        elif edit=='duplicate':case['information'][0]['name']='GpuTime'
        elif edit=='wrong_time':case['reports'][0][0]=f(1)
        elif edit=='wrong_busy':case['reports'][0][1]=u(1)
        elif edit=='truncated':case['reports'][0].pop()
        elif edit=='zero_gap':case['busy']=100
        elif edit=='zero_duration':case['busy']=100;case['reports'][0][0]=u(0)
        else:case['reports'][0][3]=f(-1)
        add('postprocess',**case)
    for index in range(600):
        qs=[dict(id=q,category=q%2,ready=[False]*rng.randrange(3)+[True],valid=rng.random()>.2,write=rng.random()>.3) for q in range(rng.randrange(5))]
        sinks_=[None]+[dict(id=100+m,category=m%3) for m in range(rng.randrange(4))]
        if qs and index%10==0:qs[-1]['throw']=rng.choice(['begin','end','ready','valid','write'])
        if len(sinks_)>1 and index%17==0:sinks_[-1]['throw']=rng.choice(['key','complete'])
        steps=[dict(op=x,wait=w) for x,w in [('poll',False),('begin',False),('begin',False),('end',False),('end',False),('poll',False),('dispatch',False),('dispatch',True),('begin',False),('end',False),('dispatch',True)]]
        add('batch',queries=qs,metrics=sinks_,key0=rng.getrandbits(64),key1=rng.getrandbits(64),tag=rng.getrandbits(32),state=rng.randrange(5),steps=steps)
    for state_ in range(4):
        add('batch',queries=[dict(id=1),None,dict(id=2)],metrics=[dict(id=100)],state=state_,steps=[dict(op='begin'),dict(op='end'),dict(op='poll'),dict(op='dispatch')])
    for index in range(800):
        slots_={str(i):[rng.choice([None,1,2,3,4]) for _ in range(rng.randrange(6))] for i in range(3)}
        accept={str(i):[rng.choice([True,False,'accept error'] if index%10==0 else [True,False]) for _ in range(4)] for i in range(1,5)}
        add('pool',slots=slots_,accept=accept,failures=rng.choice([0,1,254,255]),failure_limit=rng.choice([0,1,2,254,255]),steps=[dict(slot=rng.randrange(4),limit=rng.choice([0,1,2,2**64-1])) for _ in range(8)])
    for index in range(350):
        batches=[dict(id=i,queries=[dict(id=i*10,category=i%2,ready=[False]*rng.randrange(2)+[True],valid=rng.random()>.15)],state=rng.randrange(4),key1=i*100,tag=i) for i in range(1,7)]
        add('drain',batches=batches,slots={'0':[None,1,2],'1':[3]},deferred={'8':dict(batches=[4,5],dirty=rng.choice([True,False])),'2':dict(batches=[6],dirty=True)},metrics=[None,dict(id=100,category=0),dict(id=101,category=1)],failures=rng.choice([0,255]),failure_limit=rng.randrange(3),slot_count=rng.randrange(3),limit=rng.randrange(3),limited=rng.choice([True,False]),pending=index%19!=0,recycled=index%23!=0,clock=index%7!=0,clock_error=index%31==0,steps=[dict(op='drain',wait=False),dict(op='arm',id=8),dict(op='arm',id=8),dict(op='drain',wait=True),dict(op='arm',id=2),dict(op='drain',wait=True)])
    # Reuse saved original GPA observations without loading GPA in either process.
    original_cases=0; original_reports=0
    for name in ('report-postprocess-oracle-v2','report-postprocess-mixed','report-postprocess-rollover'):
        path=args.reference/'output/metric-scheduling'/name/'validation.json'
        saved=json.loads(path.read_text(encoding='utf-8'));assert saved['passed'] and saved['api']==8
        for case in saved['cases']:
            job=dict(op='postprocess_batches',metrics=case['metrics'],information=case['information'],batches=[b['input'] for b in case['batches']],period=1000000,samples=[[1000000,960000 if saved.get('rollover') else 0]]+[[2000000,1000]]*40,busy=case['previous_key'],normalize=case['normalize'])
            calculated=reference(copy.deepcopy(job))
            for value,observed in zip(calculated,case['batches'],strict=True):
                assert ''.join(v for row in value['reports'] for v in row)==observed['output']
                assert value['keys']==observed['keys'] and value['busy']==observed['busy_state']
                if 'clock_state' in observed:
                    assert value['state']==observed['clock_state']
                    trace=['maximum' if row[0]=='maximum' else row[:2] for row in value['trace'] if row[0]!='sample' or row[1]]
                    assert trace==observed['clock_trace']
                original_reports+=len(value['reports'])
            jobs.append(job);original_cases+=1
    expected=[]
    for job in jobs:
        try:expected.append(reference(copy.deepcopy(job)))
        except (ValueError,RuntimeError) as error:expected.append({'error':str(error)})
    request,response=args.out/'request.json',args.out/'response.json';request.write_text(json.dumps(jobs),encoding='utf-8')
    env=os.environ.copy()
    if args.isolated_env:
        env={key:value for key,value in env.items() if key.upper() in {'SYSTEMROOT','SYSTEMDRIVE','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}}
        windows=Path(os.environ['SystemRoot']);env['PATH']=str(windows/'System32')+os.pathsep+str(windows)
    env['PATH']=str(args.qt_bin.resolve())+os.pathsep+env['PATH']
    subprocess.run([str(args.exe.resolve()),'--probe',str(request.resolve()),str(response.resolve())],env=env,check=True,timeout=180)
    actual=json.loads(response.read_text(encoding='utf-8'));assert len(actual)==len(expected)
    mismatches=[dict(index=i,job=jobs[i],expected=e,actual=a) for i,(e,a) in enumerate(zip(expected,actual)) if e!=a]
    summary=dict(passed=not mismatches,cases=len(jobs),original_observation_cases=original_cases,original_observation_reports=original_reports,by_operation={op:sum(j['op']==op for j in jobs) for op in sorted({j['op'] for j in jobs})},exe_sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(),mismatches=mismatches)
    (args.out/'validation.json').write_text(json.dumps(summary,indent=2),encoding='utf-8');print(json.dumps({k:v for k,v in summary.items() if k!='mismatches'}));print('Mismatches:',len(mismatches))
    if mismatches:raise SystemExit(1)


if __name__=='__main__':main()
