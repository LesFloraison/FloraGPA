"""Compare native subscription, pool, deferred and scheduled-counter transitions."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference',type=Path,required=True);parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--qt-bin',type=Path,required=True);parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--isolated-env',action='store_true');args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    sys.path.insert(0,str(args.reference/'standalone'))
    from metric_query_pool import QueryPool,ContextSlots,pool_provider_order
    from metric_subscriptions import ProviderMetric,PublisherCollector
    from md_scheduled_pool import PublisherScheduledPool

    class Fake:
        supports_samples=True
        def __init__(self,job):
            self.supports_reuse=job.get('supported',True);self.ready_after=job.get('ready_after',0)
            self.cached=[];self.owned={};self.created=self.reused=self.serial=0;self.never=False;self.error='';self.trace=[]
        def fail(self,name):
            if self.error==name:raise RuntimeError(name)
        def sample_reserve(self,count):
            self.trace.append(['reserve',count]);self.fail('reserve')
            if self.owned or self.cached:raise RuntimeError('Invalid reserve')
            for _ in range(count):self.created+=1;self.cached.append(dict(id=self.created,uses=0,polls=0,state='cached'))
        def sample_begin(self):
            self.trace.append(['begin']);self.fail('begin')
            if not self.cached:raise RuntimeError('Empty native cache')
            item=self.cached.pop();self.reused+=item['uses']!=0;item['uses']+=1;item['polls']=0;item['state']='begun';self.serial+=1;self.owned[self.serial]=item;return self.serial
        def sample_info(self,token):self.trace.append(['info',token]);self.fail('info');return self.owned[token]['id']
        def sample_submit(self,token):
            self.trace.append(['submit',token]);self.fail('submit');item=self.owned[token]
            if item['state']!='begun':raise RuntimeError('Invalid submit')
            item['state']='ended'
        def sample_poll(self,token,flush):
            self.trace.append(['poll',token,flush]);self.fail('poll');item=self.owned[token]
            if item['state']=='begun':raise RuntimeError('Invalid poll')
            item['polls']+=1
            if not self.never and (flush or self.ready_after and item['polls']>=self.ready_after):item['state']='ready'
            return (dict(available=token%2==0),bytes([token%256])) if item['state']=='ready' else None
        def sample_recycle(self,token):
            self.trace.append(['recycle',token]);self.fail('recycle');item=self.owned[token]
            if item['state']!='ready':raise RuntimeError('Invalid recycle')
            del self.owned[token];self.cached.append(item)
        def sample_release(self,token):
            self.trace.append(['release',token]);self.fail('release')
            if token not in self.owned:raise RuntimeError('Unknown release token')
            del self.owned[token]
        def sample_clear_cache(self):
            self.trace.append(['clear']);self.fail('clear')
            if self.owned:raise RuntimeError('Live native counters')
            self.cached.clear()
        def stats(self):return dict(created=self.created,reused=self.reused,cached=len(self.cached),owned=len(self.owned))
        def sample_stats(self):self.trace.append(['stats']);self.fail('stats');return self.stats()

    class Publisher:
        def __init__(self,fake):self.fake=fake;self.records=[];self.refreshes=[]
        def update(self,force):
            self.fake.trace.append(['clock',force]);self.fake.fail('clock')
            if any(v['state']=='begun' for v in self.fake.owned.values()):raise RuntimeError('Clock during begun sample')
            self.refreshes.append(len(self.records))

    def scheduled(job):
        fake=Fake(job);publisher=Publisher(fake);pool=PublisherScheduledPool(fake,publisher,job.get('capacity',3),job.get('timeout',10000));rows=[];consumer_error=False
        def consume(result,raw):
            fake.trace.append(['consume',raw[0]])
            if consumer_error:raise RuntimeError('consumer')
            publisher.records.append([result['available'],raw[0]])
        for step in job['steps']:
            row={}
            try:
                op=step['op'];value=None
                if op=='fault':fake.error=step['value']
                elif op=='never':fake.never=step['value']
                elif op=='consumer_error':consumer_error=step['value']
                elif op=='begin':pool.begin(consume if step.get('consume',True) else None)
                elif op=='drain':pool.drain(step.get('wait',False))
                elif op=='end':r,b=pool.end();value=dict(values=r,raw=list(b))
                else:getattr(pool,op)()
                row['return']=value
            except (RuntimeError,ValueError,TimeoutError) as e:row['error']=str(e)
            row.update(report=copy.deepcopy(pool.report()),owned=len(pool.owned),active=pool.current is not None,stats=fake.stats(),records=copy.deepcopy(publisher.records),refreshes=publisher.refreshes[:],trace=copy.deepcopy(fake.trace));fake.trace.clear();rows.append(row)
        return rows

    class Query:
        def __init__(self,name,category,cfg,trace):self.id=name;self.category=category;self.cfg=cfg;self.trace=trace;self.attempt=0
        def begin(self,context):self.trace.append(['begin',self.id,context]);self.attempt=0
        def end(self,context):self.trace.append(['end',self.id,context])
        def ready(self,flush):
            self.trace.append(['ready',self.id,flush]);values=self.cfg.get('ready',[True]);value=values[min(self.attempt,len(values)-1)];self.attempt+=1;return value
        def valid(self):self.trace.append(['valid',self.id]);return self.cfg.get('valid',True)
        def write_metric(self,metric):self.trace.append(['write',self.id,metric.id]);return self.cfg.get('write',True)
    class Provider:
        def __init__(self,cfg,trace):self.id=cfg['id'];self.category=cfg['category'];self.cfg=cfg;self.trace=trace;self.count=0
        def create(self):
            self.count+=1;self.trace.append(['create',self.id,self.count])
            if self.cfg.get('throw_at')==self.count:raise RuntimeError('create '+self.id)
            if self.cfg.get('null_every') and self.count%self.cfg['null_every']==0:return None
            return Query(self.id+'#'+str(self.count),self.category,self.cfg,self.trace)
    class Metric(ProviderMetric):
        def __init__(self,cfg,providers,trace):super().__init__([providers[p] for p in cfg['providers']],cfg.get('selected',0),cfg.get('compatible'));self.id=cfg['id'];self.trace=trace
        def set_mode(self,mode):self.trace.append(['mode',self.id,mode]);super().set_mode(mode)
        def bind(self,kind):self.trace.append(['bind',self.id,kind]);return super().bind(kind)
        def set_key(self,key):self.trace.append(['key',self.id,key])
        def complete(self,key,tag):self.trace.append(['complete',self.id,key,tag])
        def flush(self):self.trace.append(['flush',self.id])
    def describe(batch):return None if batch is None else dict(queries=[q.id for q in batch.queries],state=batch.state,key0=batch.key0,key1=batch.key1,tag=batch.tag)
    def slots(values):return {str(k):[describe(b) for b in items] for k,items in values.items()}
    def reference(job):
        if job['op']=='scheduled':return scheduled(job)
        trace=[];rows=[];providers={};ordered=[]
        for cfg in job['providers']:
            if cfg is None:ordered.append(None);continue
            p=Provider(cfg,trace);providers[p.id]=p;ordered.append(p)
        counts=lambda:{k:p.count for k,p in providers.items()}
        if job['op']=='order':
            try:return [p.id for p in pool_provider_order(ordered)]
            except AttributeError:raise RuntimeError('Missing query provider') from None
        if job['op']=='pool':
            pool=QueryPool(ordered,job.get('capacity',0));handles={}
            for step in job['steps']:
                row={}
                try:
                    slot=step.get('slot',0);op=step['op'];value=None
                    if op=='empty':value=pool.empty(slot)
                    elif op=='capacity':pool.capacity=step['value']
                    elif op=='append':pool.append(slot,None if step['handle'] is None else handles[step['handle']])
                    else:
                        b=pool.take(slot,step.get('replenish',False)) if op=='take' else pool.take_first(slot) if op=='first' else pool.peek(slot)
                        if 'save' in step:handles[step['save']]=b
                        value=describe(b)
                    row['return']=value
                except (RuntimeError,ValueError) as e:row['error']=str(e)
                row.update(slots=slots(pool.slots),counts=counts(),trace=trace[:]);trace.clear();rows.append(row)
            return rows
        def context_type(value):trace.append(['context_type',value]);return job['context_types'][str(value)]
        contexts=ContextSlots(job.get('contexts',[]),context_type)
        if job['op']=='contexts':
            for value in job['values']:
                index=contexts.slot(value);rows.append(dict(**{'return':index},contexts=contexts.contexts[:],trace=trace[:]));trace.clear()
            return rows
        metrics={cfg['id']:Metric(cfg,providers,trace) for cfg in job['metrics']}
        class Clock:
            def update(self,force):trace.append(['clock',force])
        collector=PublisherCollector(contexts,Clock(),job.get('capacity',3),job.get('mode',False),job.get('preferred'),job.get('defer_reads',False))
        for step in job['steps']:
            row={}
            try:
                op=step['op'];value=None
                if op in ('subscribe','unsubscribe'):getattr(collector,op)(metrics[step['id']])
                elif op=='capacity':collector.set_capacity(step['value'])
                elif op=='begin':value=describe(collector.begin(step.get('key0',11),step.get('key1',22),step.get('tag',6),step.get('context',0)))
                elif op=='end':collector.end(step.get('context',0))
                elif op=='drain':collector.drain.drain(step.get('wait',False))
                elif op=='notify':collector.notifications.notify(step.get('context',0),step['key'],step['kind'])
                elif op=='defer':collector.defer_reads=step['value']
                elif op=='rebuild':collector.rebuild()
                row['return']=value
            except (RuntimeError,ValueError) as e:row['error']=str(e)
            row.update(generation=collector.generation,capacity=collector.capacity,contexts=contexts.contexts[:],slot_count=collector.drain.slot_count,
                providers=[p.id for p in collector.subscriptions.providers],metrics=[m.id for m in collector.subscriptions.metrics],compatible=sorted(collector.subscriptions.compatible),
                selection={k:dict(provider=m.provider.id,selected=m.selected,mode=m.mode) for k,m in metrics.items()},recycled=None if collector.recycled is None else slots(collector.recycled.slots),
                recording=None if collector.recording is None else slots(collector.recording.slots),pending=None if collector.drain.pending is None else dict(slots=slots(collector.drain.pending.slots),failures=collector.drain.pending.failures,failure_limit=collector.drain.pending.failure_limit),
                deferred={str(k):dict(batches=[describe(b) for b in d.batches],dirty=d.dirty,source_slot=d.source_slot) for k,d in collector.drain.deferred.items()},counts=counts(),trace=trace[:]);trace.clear();rows.append(row)
        return rows

    rng=random.Random(0x217a0);jobs=[]
    def add(op,**values):jobs.append(dict(op=op,**values))
    def action(op,**values):return dict(op=op,**values)
    for kinds in ([],[0],[-1],[0,-1,1,-1],[-2,-1,-2147483648,2147483647,0],[-1,-1]):add('order',providers=[dict(id=str(i),category=k) for i,k in enumerate(kinds)])
    add('order',providers=[None])
    for index in range(160):
        providers=[None]+[dict(id='p'+str(i),category=rng.choice([-1,0,1]),null_every=rng.randrange(4),throw_at=rng.choice([0,0,0,2,5])) for i in range(rng.randrange(4))]
        steps=[action('append',slot=99,handle=None),action('peek',slot=0),action('take',slot=0,replenish=True,save='a'),action('first',slot=0,save='b'),action('append',slot=1,handle='a'),action('take',slot=1,save='c'),action('append',slot=0,handle='b'),action('empty',slot=4),action('take',slot=0,replenish=True)]
        # Failed creation does not produce a saved handle; use separate valid refills here.
        for p in providers:
            if p:p['throw_at']=0
        add('pool',providers=providers,capacity=rng.choice([0,1,2,4]),steps=steps)
    for capacity in (0,1,2,2**32,2**64-1):
        add('pool',providers=[dict(id='p',category=0,throw_at=2)],capacity=capacity,steps=[action('take',replenish=True),action('take',replenish=True),action('take'),action('empty')])
    for initial in ([],[0],[10,20],[20,10],[20,20,0]):add('contexts',providers=[],contexts=initial,context_types={'10':0,'20':1,'30':0,'40':2},values=[0,10,20,0,30,30,40,0])
    for index in range(250):
        categories=[0,0,1,2,-1,-1,-2147483648,2147483647]
        providers=[dict(id='p'+str(i),category=k,ready=[False]*rng.randrange(2)+[True],write=rng.choice([True,False])) for i,k in enumerate(categories)]
        metrics=[]
        for m in range(4):
            choices=rng.sample([p['id'] for p in providers],rng.randrange(1,5));cfg=dict(id='m'+str(m),providers=choices,selected=rng.randrange(len(choices)))
            if index%4==0:cfg['compatible']=rng.sample([0,1,2,42,2**32-1,2**31],rng.randrange(4))
            metrics.append(cfg)
        steps=[]
        for _ in range(12):steps.append(action(rng.choice(['subscribe','subscribe','unsubscribe']),id='m'+str(rng.randrange(4))))
        steps += [action('begin'),action('end'),action('drain',wait=True),action('capacity',value=2),action('begin'),action('end'),action('drain',wait=True)]
        extra={'preferred':rng.choice([0,1,2,42,2**32-1,2**31])} if index%3==0 else {}
        add('collector',providers=providers,metrics=metrics,capacity=rng.randrange(1,4),contexts=[],context_types={},mode=bool(index%2),steps=steps,**extra)
    for defer in (False,True):
        for delay in (0,1,3):
            for capacity in (0,1,3):
                steps=[action('subscribe',id='m')]
                for context in (10,20,20,30):steps.extend([action('begin',context=context,key0=context,key1=context*10),action('end',context=context)])
                steps += [action('notify',context=20,key=77,kind=2),action('notify',context=20,key=77,kind=3),action('notify',context=20,key=77,kind=3),action('drain',wait=True),action('notify',key=77,kind=0),action('notify',key=999,kind=0),action('notify',key=999,kind=9),action('drain',wait=True)]
                add('collector',providers=[dict(id='p',category=0,ready=[False]*delay+[True])],metrics=[dict(id='m',providers=['p'])],contexts=[20,10],context_types={'10':0,'20':1,'30':0},capacity=capacity,defer_reads=defer,steps=steps)
    # Duplicate seal consumes its new list; dirty release moves work to pending.
    steps=[action('subscribe',id='m'),action('begin',context=20),action('end',context=20),action('notify',context=20,key=7,kind=2),action('begin',context=20),action('end',context=20),action('notify',context=20,key=7,kind=2),action('notify',key=7,kind=3),action('notify',key=7,kind=0),action('drain',wait=True),action('begin',context=20),action('end',context=20),action('notify',context=20,key=8,kind=2),action('rebuild'),action('notify',key=8,kind=3),action('drain',wait=True),action('notify',key=8,kind=0)]
    add('collector',providers=[dict(id='p',category=0)],metrics=[dict(id='m',providers=['p'])],context_types={'20':1},capacity=2,defer_reads=True,steps=steps)
    # A failed fallback dispatch retains its old state, even when Begin changes its keys.
    for valid in (False,True):
        for defer in (False,True):
            steps=[action('subscribe',id='m'),action('begin',key0=2**64-1,key1=2**63,tag=2**32-1),action('end'),action('begin',key0=17,key1=19),action('end')]
            steps += [action('drain',wait=True)]*5
            steps += [action('unsubscribe',id='m'),action('begin'),action('end'),action('drain',wait=True)]
            add('collector',providers=[dict(id='p',category=0,valid=valid)],metrics=[dict(id='m',providers=['p'])],capacity=1,defer_reads=defer,steps=steps)
    for throw_at in (1,2,3):
        add('collector',providers=[dict(id='p',category=0,throw_at=throw_at)],metrics=[dict(id='m',providers=['p'])],capacity=3,steps=[action('subscribe',id='m'),action('begin'),action('begin'),action('end'),action('drain',wait=True)])
    for ready in (0,1,2,5):
        for capacity in (1,2,3,7):
            for count in (1,3,9,25):
                steps=[]
                for _ in range(count):steps.extend([action('begin'),action('submit')])
                steps += [action('finish'),action('close'),action('begin',consume=False),action('end'),action('close')]
                add('scheduled',capacity=capacity,ready_after=ready,steps=steps)
    for fault in ('reserve','begin','info','submit','poll','recycle','clock','consumer'):
        steps=[]
        if fault not in ('reserve','begin','info','submit'):
            for _ in range(3):steps.extend([action('begin'),action('submit')])
        steps += [action('consumer_error',value=True) if fault=='consumer' else action('fault',value=fault)]
        if fault in ('reserve','begin','info'):steps.append(action('begin'))
        elif fault=='submit':steps.extend([action('begin'),action('submit')])
        else:steps.append(action('finish'))
        steps += [action('fault',value=''),action('consumer_error',value=False),action('begin'),action('end'),action('close')]
        add('scheduled',capacity=3,steps=steps)
    add('scheduled',capacity=3,steps=[action('begin'),action('end'),action('fault',value='begin'),action('begin'),action('fault',value=''),action('begin'),action('end'),action('close')])
    add('scheduled',capacity=2,steps=[action('submit'),action('end'),action('begin'),action('begin'),action('drain'),action('finish'),action('end'),action('close')])
    # Cleanup failures preserve the original exception precedence and residual ownership.
    for fault in ('release','clear','stats'):
        steps=[action('begin'),action('submit'),action('begin'),action('submit'),action('fault',value=fault),action('close'),action('fault',value=''),action('close')]
        if fault!='release':steps += [action('begin'),action('end'),action('close')]
        add('scheduled',capacity=3,steps=steps)
    for capacity in (0,257):add('scheduled',capacity=capacity,steps=[])
    for timeout in (0,60001):add('scheduled',timeout=timeout,steps=[])
    add('scheduled',supported=False,steps=[])
    expected=[]
    for job in jobs:
        try:expected.append(reference(copy.deepcopy(job)))
        except (ValueError,RuntimeError) as e:expected.append({'error':str(e)})
    request,response=args.out/'request.json',args.out/'response.json';request.write_text(json.dumps(jobs),encoding='utf-8')
    env=os.environ.copy()
    if args.isolated_env:
        env={k:v for k,v in env.items() if k.upper() in {'SYSTEMROOT','SYSTEMDRIVE','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}};windows=Path(os.environ['SystemRoot']);env['PATH']=str(windows/'System32')+os.pathsep+str(windows)
    env['PATH']=str(args.qt_bin.resolve())+os.pathsep+env['PATH']
    subprocess.run([str(args.exe.resolve()),'--probe',str(request.resolve()),str(response.resolve())],env=env,check=True,timeout=180)
    actual=json.loads(response.read_text(encoding='utf-8'));assert len(actual)==len(expected)
    mismatches=[dict(index=i,job=jobs[i],expected=e,actual=a) for i,(e,a) in enumerate(zip(expected,actual)) if e!=a]
    summary=dict(passed=not mismatches,cases=len(jobs),by_operation={op:sum(j['op']==op for j in jobs) for op in sorted({j['op'] for j in jobs})},exe_sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(),mismatches=mismatches)
    (args.out/'validation.json').write_text(json.dumps(summary,indent=2),encoding='utf-8');print(json.dumps({k:v for k,v in summary.items() if k!='mismatches'}));print('Mismatches:',len(mismatches))
    if mismatches:raise SystemExit(1)


if __name__=='__main__':main()
