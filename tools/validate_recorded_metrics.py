"""Compare owned recorded sessions and publisher transforms with the unchanged Python reference."""
import argparse
import copy
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import struct
import sys
import tempfile


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('reference', 'exe', 'qt-bin', 'out'): p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--isolated-env', action='store_true')
    args = p.parse_args(); args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.reference/'standalone'))
    import md_recorded_queries as recorded
    from md_publisher_values import PublisherValues, RecordedPublisherValues

    class Fake:
        def __init__(self, job):
            self.cfg=job; self.selected=copy.deepcopy(job.get('metadata')); self.trace=[]; self.owned={}; self.commands={}
            self.token=self.execution=self.command_index=self.clock_reads=0
            self.fault=''; self.never=self.clock_failed=self.clock_changed=False; self.supports_recorded=job.get('supported',True)
        def fail(self,name):
            if name in self.fault.split(','): raise RuntimeError('Injected '+name)
        def provenance(self): return dict(fake=True)
        def clock_pair(self):
            self.trace.append(['clock']); self.fail('clock'); index=self.clock_reads; self.clock_reads+=1
            if 'clocks' in self.cfg:return copy.deepcopy(self.cfg['clocks'][min(index,len(self.cfg['clocks'])-1)])
            return dict(maximum_ns=1000001 if self.clock_changed else 1000000,frequency_hz=1000,status=int(self.clock_failed),gpu_ns=100+index*10,cpu_ns=1000+index*10)
        def method(self,key,index,*_):
            if index==112:
                def kind(context):self.trace.append(['context_type',context]);return 1
                return kind
            if index==114:
                def finish(context,restore,output):
                    self.trace.append(['finish',context,restore]);self.fail('finish')
                    keys=self.cfg.get('command_keys',[500,100,300,900,200,800,600,400]);key=keys[self.command_index] if self.command_index<len(keys) else 1000+self.command_index
                    self.command_index+=1;self.commands[key]=context;c.cast(output,c.POINTER(c.c_void_p))[0]=key;return 0
                return finish
            assert index==2
            def release(command):
                self.trace.append(['release_command',command]);self.fail('release_command')
                if command not in self.commands:raise RuntimeError('Unknown command')
                del self.commands[command];return 0
            return release
        def recorded_begin(self,context):
            self.trace.append(['begin',context]);self.fail('begin')
            if any(v['context']==context and v['state']=='begun' for v in self.owned.values()):raise RuntimeError('Overlapping native counter')
            self.token+=1;self.owned[self.token]=dict(context=context,execution=0,polls=0,state='begun');return self.token
        def recorded_end(self,token):
            self.trace.append(['end',token]);self.fail('end');counter=self.owned[token]
            if counter['state']!='begun':raise RuntimeError('Invalid native end')
            counter['state']='ended'
        def recorded_execute(self,key,tokens,restore):
            self.trace.append(['execute',key,tokens,restore]);self.fail('execute')
            if any(self.owned[t]['context']!=self.commands[key] or self.owned[t]['state']=='begun' for t in tokens):raise RuntimeError('Invalid execution roster')
            self.execution+=1
            for token in tokens:self.owned[token].update(execution=self.execution,polls=0,state='executed')
            return self.execution
        def recorded_poll(self,token,execution,flush):
            self.trace.append(['poll',token,execution,flush]);self.fail('poll');counter=self.owned[token]
            if not execution or counter['execution']!=execution:raise RuntimeError('Stale execution')
            counter['polls']+=1;ready=self.cfg.get('ready_after',2)
            if not self.never and (flush or ready and counter['polls']>=ready):counter['state']='ready'
            if counter['state']!='ready':return None
            raw=bytes([token%256,execution%256,counter['context']%256,0])
            if 'result' in self.cfg:return copy.deepcopy(self.cfg['result']),raw
            key=execution*100+token;available=token%3!=0
            return dict(available=available,unavailable_reasons=[] if available else ['ReportLost'],reports=1,values=[dict(type=1,value=1000+token),dict(type=2,value=2.5),dict(type=1,value=token*3),dict(type=1,value=key),dict(type=1,value=key+100),dict(type=3,value=not available)]),raw
        def recorded_release(self,token):
            self.trace.append(['release',token]);self.fail('release')
            if token not in self.owned:raise RuntimeError('Unknown token')
            del self.owned[token]
        def native(self):return dict(counters=[[k,v['context'],v['execution'],v['polls'],v['state']] for k,v in sorted(self.owned.items())],commands=[[k,v] for k,v in sorted(self.commands.items())])

    def reference(job):
        fake=Fake(job);rows=[]
        with tempfile.TemporaryDirectory() as temp:
            if job.get('op')=='publisher':
                publisher=(RecordedPublisherValues if job.get('recorded') else PublisherValues)(fake)
                for step in job['steps']:
                    row={}
                    try:
                        op=step['op']
                        if op=='update':publisher.update(step.get('force',False))
                        elif op=='append':publisher.append(step['metadata'],step['row'])
                        elif op=='fault':fake.fault=step['value']
                        elif op=='clock_fail':fake.clock_failed=step['value']
                        elif op=='clock_changed':fake.clock_changed=step['value']
                        row['return']=None
                    except (ValueError,RuntimeError,OverflowError,struct.error) as e:row['error']=str(e)
                    csv=Path(temp)/'publisher.csv';publisher.write_csv(csv)
                    row.update(report=copy.deepcopy(publisher.report()),csv=csv.read_bytes().decode('utf-8'),trace=copy.deepcopy(fake.trace));fake.trace.clear();rows.append(row)
                return rows
            previous=recorded.method;recorded.method=fake.method
            try:
                session=recorded.RecordedCounterLists(fake,job.get('timeout',10000),job.get('publisher',False));handles={};export_index=0
                for step in job['steps']:
                    row={};context=step.get('context',10);key=handles.get(step['handle'],0) if 'handle' in step else step.get('key',0);op=step['op']
                    try:
                        value=None
                        if op=='begin':session.begin(context,step.get('key0',1),step.get('key1'),step.get('tag',6))
                        elif op=='end':session.end(context)
                        elif op=='finish':value=session.finish(context,step.get('restore',False));handles[step['save']]=value
                        elif op=='execute':value=session.execute(key,step.get('restore',False))
                        elif op=='release':session.release(key)
                        elif op=='drain':session.drain(step.get('wait',True))
                        elif op=='close':session.close()
                        elif op=='fault':fake.fault=step['value']
                        elif op=='never':fake.never=step['value']
                        elif op=='clock_fail':fake.clock_failed=step['value']
                        elif op=='clock_changed':fake.clock_changed=step['value']
                        elif op=='metadata':fake.selected=copy.deepcopy(step['value'])
                        elif op=='report':value=session.report()
                        elif op=='export':
                            export_index+=1;folder=Path(temp)/str(export_index);value=session.export(folder)
                            row['export']=dict(raw=(folder/'raw-values.csv').read_bytes().hex(),json=json.loads((folder/'recorded-profile.json').read_text(encoding='utf-8')),publisher=(folder/'publisher-values.csv').read_bytes().hex() if (folder/'publisher-values.csv').exists() else None)
                        row['return']=value
                    except (ValueError,RuntimeError,TimeoutError) as e:row['error']=str(e)
                    row.update(state=dict(closed=session.closed,failed=session.failed,abandoned=session.abandoned,owned=len(session.owned),commands=len(session.commands)),records=copy.deepcopy(session.records),native=fake.native(),trace=copy.deepcopy(fake.trace));fake.trace.clear();rows.append(row)
                return rows
            finally:recorded.method=previous

    metadata=dict(name='Compute,"测\n试',report_size=4,metrics=[dict(name='GpuTime',unit='ns',metric_type=0),dict(name='GpuBusy',unit='percent',metric_type=0),dict(name='CsThreads',unit='threads',metric_type=0)],information=[dict(name='QueryBeginTime',unit='ns',info_type=3),dict(name='QueryEndTime',unit='ns',info_type=3),dict(name='ReportLost',unit='',info_type=0)])
    jobs=[];rng=random.Random(0x213a0)
    def action(op,**values):return dict(op=op,**values)
    def add(steps,**values):jobs.append(dict(metadata=copy.deepcopy(metadata),steps=steps,**values))
    for publisher in (False,True):
        for ready in (0,1,2,4):
            for count in (1,2,3,5):
                for repeat in (1,2,3):
                    steps=[]
                    for index in range(count):
                        context=10+(index%3)*10
                        for ordinal in range(2):steps.extend([action('begin',context=context,key0=index*2+ordinal,key1=100+index*2+ordinal),action('end',context=context)])
                        steps.append(action('finish',context=context,save=str(index),restore=bool(index%2)))
                    for iteration in range(repeat):
                        order=list(range(count));rng.shuffle(order)
                        for index in order:
                            steps.extend([action('execute',handle=str(index),restore=bool((index+iteration)%2)),action('report')])
                            if index%2:steps.append(action('drain',wait=False))
                    for index in range(count):steps.append(action('release',handle=str(index)))
                    steps += [action('drain'),action('export'),action('close'),action('report'),action('close')]
                    add(steps,publisher=publisher,ready_after=ready)
        # Live contexts, empty and unexecuted lists, and metadata changes after release.
        steps=[action('begin',context=10,key0=2**64-1,key1=2**63,tag=2**32-1),action('begin',context=20),action('report'),action('end',context=20),action('finish',context=20,save='b'),action('end',context=10),action('finish',context=10,save='a'),action('release',handle='b'),action('finish',context=20,save='empty'),action('execute',handle='empty'),action('release',handle='empty'),action('execute',handle='a'),action('drain'),action('release',handle='a')]
        alternate=copy.deepcopy(metadata);alternate['name']='Other'
        steps += [action('metadata',value=alternate),action('begin'),action('end'),action('finish',save='new'),action('execute',handle='new'),action('drain'),action('release',handle='new'),action('export'),action('close'),action('report')]
        add(steps,publisher=publisher)
        for fault in ('begin','end','finish','execute','poll','release','release_command','clock'):
            if fault=='clock' and not publisher:continue
            steps=[]
            if fault!='begin':steps.append(action('begin'))
            if fault not in ('begin','end'):steps.append(action('end'))
            if fault not in ('begin','end','finish'):steps.append(action('finish',save='a'))
            if fault in ('poll','clock'):steps.append(action('execute',handle='a'))
            target={'begin':action('begin'),'end':action('end'),'finish':action('finish',save='a'),'execute':action('execute',handle='a'),'poll':action('drain'),'release':action('release',handle='a'),'release_command':action('release',handle='a'),'clock':action('drain')}[fault]
            steps += [action('fault',value=fault),target,action('report'),action('close'),action('begin')]
            add(steps,publisher=publisher)
        for state in ('active','recording','sealed','dirty','drained'):
            steps=[action('begin')]
            if state!='active':steps.append(action('end'))
            if state in ('sealed','dirty','drained'):steps.append(action('finish',save='a'))
            if state in ('dirty','drained'):steps.append(action('execute',handle='a'))
            if state=='drained':steps.append(action('drain'))
            add(steps+[action('close'),action('report'),action('export'),action('drain')],publisher=publisher)
        changed=copy.deepcopy(metadata);changed['report_size']=8
        add([action('end'),action('finish',save='x'),action('execute'),action('release'),action('begin'),action('begin'),action('finish',save='x'),action('end'),action('metadata',value=changed),action('begin'),action('close'),action('report')],publisher=publisher)
    for flag in ('clock_fail','clock_changed'):
        add([action('begin'),action('end'),action('finish',save='a'),action('execute',handle='a'),action(flag,value=True),action('drain'),action('report')],publisher=True)
    add([action('begin')],metadata_override=None)  # replaced below to exercise no selected set
    jobs[-1]['metadata']=None;jobs[-1].pop('metadata_override')
    for timeout in (0,60001):add([],timeout=timeout)
    add([],supported=False)
    # The publisher classes are checked independently of command-list collection.
    def pair(gpu=100,cpu=1000,status=0,maximum=1000000):return dict(maximum_ns=maximum,frequency_hz=1000,status=status,gpu_ns=gpu,cpu_ns=cpu)
    for recorded_mode in (False,True):
        for index in range(100):
            values=[dict(type=1,value=rng.randrange(1,100000)),dict(type=2,value=rng.uniform(0,100)),dict(type=1,value=rng.randrange(2**64)),dict(type=1,value=rng.randrange(100,900000)),dict(type=1,value=rng.randrange(100,900000)),dict(type=3,value=bool(index%2))]
            if recorded_mode:row=dict(set=metadata['name'],key0=index,key1=index+1,tag=6,token=index+1,execution=1,list_id=1,context_slot=0,raw_hex='01020304',result=dict(values=values,available=bool(index%2),unavailable_reasons=[]))
            else:row=dict(set=metadata['name'],event=index,pass_index=0,sample_index=0,raw_report='r.bin',raw_sha256='a'*64,available=bool(index%2),unavailable_reasons=[],values=values[:3],information=values[3:])
            steps=[action('append',metadata=metadata,row=row),action('update',force=bool(index%2)),action('append',metadata=metadata,row=row)]
            if index%5==0:steps += [action('clock_fail',value=True),action('update'),action('clock_fail',value=False),action('update'),action('append',metadata=metadata,row=row)]
            add(steps,op='publisher',recorded=recorded_mode)
        for clocks in ([pair(status=1)]*3,[pair(status=1),pair(status=1),pair()],[pair(),pair(maximum=999999)],[pair(980000,981000),pair(10,1000010)]):
            add([action('update'),action('update')],op='publisher',recorded=recorded_mode,clocks=clocks)
    expected=[]
    template=next(copy.deepcopy(j) for j in jobs if j.get('op')=='publisher' and j.get('recorded'))
    for raw in ('', '000102', '00 01\t02\n03', 'g0010203', '0 010203', '0001020'):
        job=copy.deepcopy(template);job['steps']=job['steps'][:1];job['steps'][0]['row']['raw_hex']=raw;jobs.append(job)
    for scalar in (None,-1,1.5,True,2**64-1):
        job=copy.deepcopy(template);job['steps']=job['steps'][:1];job['steps'][0]['row']['result']['values'][2]['value']=scalar;jobs.append(job)
    for raw_size in (3,5):
        job=copy.deepcopy(template);job['steps']=job['steps'][:1];job['steps'][0]['metadata']['report_size']=raw_size;jobs.append(job)
    for recorded_mode in (False,True):
        for failure in ('none','retry','metadata'):
            job=next(copy.deepcopy(j) for j in jobs if j.get('op')=='publisher' and j.get('recorded')==recorded_mode)
            step=job['steps'][0];row=step['row']
            if recorded_mode:row['result']['values'][3]['value']=10;row['result']['values'][4]['value']=20
            else:row['information'][0]['value']=10;row['information'][1]['value']=20
            clocks=[pair(980000,1980000)]
            if failure=='retry':clocks += [pair(status=1)]*3
            if failure=='metadata':clocks += [pair(10,2000010,maximum=999999)]
            clocks += [pair(10,2000010)]
            job['clocks']=clocks;job['steps']=[step,action('update'),copy.deepcopy(step)];jobs.append(job)
    for job in jobs:
        try:expected.append(reference(copy.deepcopy(job)))
        except (ValueError,RuntimeError) as e:expected.append(dict(error=str(e)))
    request,response=args.out/'request.json',args.out/'response.json';request.write_text(json.dumps(jobs),encoding='utf-8')
    env=os.environ.copy()
    if args.isolated_env:
        env={k:v for k,v in env.items() if k.upper() in {'SYSTEMROOT','SYSTEMDRIVE','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}};windows=Path(os.environ['SystemRoot']);env['PATH']=str(windows/'System32')+os.pathsep+str(windows)
    env['PATH']=str(args.qt_bin.resolve())+os.pathsep+env['PATH']
    subprocess.run([str(args.exe.resolve()),'--probe',str(request.resolve()),str(response.resolve())],env=env,check=True,timeout=180)
    actual=json.loads(response.read_text(encoding='utf-8'));assert len(actual)==len(expected)
    mismatches=[dict(index=i,job=jobs[i],expected=e,actual=a) for i,(e,a) in enumerate(zip(expected,actual)) if e!=a]
    result=dict(passed=not mismatches,cases=len(jobs),recorded_cases=sum(j.get('op')!='publisher' for j in jobs),publisher_cases=sum(j.get('op')=='publisher' for j in jobs),exe_sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(),mismatches=mismatches)
    (args.out/'validation.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps({k:v for k,v in result.items() if k!='mismatches'}));print('Mismatches:',len(mismatches))
    if mismatches:raise SystemExit(1)


if __name__=='__main__':main()
