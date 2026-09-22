"""Exact iteration protocol parity and replay of saved GPA numeric observations."""
import argparse
import copy
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
from validate_metric_analysis import encode, wire


def read(path):return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def wire_records(rows):
    plans=[];responses=[]
    for row in rows:
        data=bytes.fromhex(row['response']);offset=0
        def take(fmt):
            nonlocal offset
            out=struct.unpack_from(fmt,data,offset);offset+=struct.calcsize(fmt);return out
        if row['command']==7:
            _,flag,count=take('<QBQ');groups=[]
            for _ in range(count):
                size=take('<Q')[0];groups.append(list(take('<'+'I'*(size//4))))
            plans.append(dict(groups=groups,flag=bool(flag)))
        else:
            result=dict(metrics=[],parallel_ranges=[],flag=False)
            while True:
                tag=take('<B')[0]
                if tag==0:result['flag']=bool(take('<B')[0]);break
                if tag==7:
                    mid,size=take('<QQ');values=list(take('<'+'d'*(size//8)));size=take('<Q')[0];aux=list(take('<'+'Q'*(size//8)))
                    result['metrics'].append(dict(metric=mid,values=values,aux=aux))
                elif tag==8:result['parallel_ranges'].append([list(take('<QQ')) for _ in range(take('<Q')[0])])
                else:raise AssertionError(tag)
            responses.append(result)
        assert offset==len(data)
    return plans,responses


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['reference','exe','qt-bin','out']:p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--cli',type=Path)
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False);sys.path.insert(0,str(a.reference/'standalone'))
    import metric_iterations as mi
    from metric_outer_passes import MetricOuterPassRunner,select_metric_passes
    from metric_range_mapping import map_metric_ranges,select_metric_range_table
    from metric_query_flags import parse_query_flags
    from frame_metric_index import build_metric_index,api_kind,SUPPORTED_TYPES
    from frame import Frame
    jobs=[];expected=[];labels=[];oracles=[];sources=[];rng=random.Random(391461)
    class Transport:
        def __init__(self,j):self.j=j;self.trace=[];self.requested=[];self.serial=0;self.prepares=0;self.polls=0
        def fail(self,op):
            if self.j.get('throw')==op:raise RuntimeError('injected '+op)
        def descriptions(self):self.trace.append(['catalog']);self.fail('catalog');return self.j.get('descriptions',[])
        def query_flag_descriptions(self):
            if 'flag_descriptions' in self.j:self.trace.append(['flag_catalog'])
            self.fail('flag_catalog');return self.j.get('flag_descriptions',[])
        def prepare(self,ids):
            self.trace.append(['prepare',list(ids)]);self.fail('prepare');self.requested=list(ids)
            if 'plans' in self.j:
                plan=self.j['plans'][min(self.prepares,len(self.j['plans'])-1)];self.prepares+=1;return plan['groups'],plan['flag']
            return [[i] for i in range(len(ids))],self.j.get('prepare_flag',False)
        def replay(self,index,ranges,flag):
            self.trace.append(['replay',index,self.requested[:],[list(r) for r in ranges],flag]);self.fail('replay')
            if 'responses' in self.j:
                response=copy.deepcopy(self.j['responses'][min(self.serial,len(self.j['responses'])-1)]);self.serial+=1;return response
            self.serial+=1;metrics=[];parallel=[]
            if self.requested:
                mid=self.requested[index];metrics=[dict(metric=mid,values=[float(mid)+self.serial*.5+r*3 for r in range(len(ranges))],aux=[1<<((self.serial+r)%64) for r in range(len(ranges))])]
            policy=self.j.get('policy','normal');weight=bool(self.requested) and self.requested[index]<42
            if (weight and policy=='weight_empty') or (weight and policy=='weight_empty_first' and self.serial==1) or (weight and policy=='weight_empty_twice' and self.serial<=2) or (not weight and policy=='main_empty') or (not weight and policy=='main_empty_first' and self.serial==1):metrics=[]
            if metrics:
                if (weight and policy=='weight_wrong_range') or (not weight and policy=='main_wrong_range'):metrics[0]['values'].append(99.)
                if weight and policy=='weight_wrong_id':metrics[0]['metric']=999
                if weight and policy=='weight_multiple':metrics.append(copy.deepcopy(metrics[0]))
            timing=self.j.get('timing','absent');pairs=[[self.serial*100+r*19,5+r] for r in range(len(ranges))]
            if timing=='ordinary':parallel=[pairs]
            if timing=='empty':parallel=[[]]
            if timing=='multiple':parallel=[pairs,[[901,902]]]
            if timing=='boundary':parallel=[[[2**64-1,2**64-2],[2**63,2**53+1]]]
            return dict(metrics=metrics,parallel_ranges=parallel,flag=True)
        def cancel(self):
            target=self.j.get('cancel_at');value=target=='always' or target==self.polls;self.polls+=1;self.trace.append(['cancel',value]);return value
    def reference(j):
        op=j['op']
        if op=='decoder_types':return sorted(SUPPORTED_TYPES)
        if op=='select':
            r=select_metric_passes(j['groups'],j['requested'],j['flag']);return dict(first=r.start,count=len(r))
        if op=='range_table':return select_metric_range_table({int(k):v for k,v in j['categorized'].items()},j.get('fallback',[]))
        if op=='ranges':return [list(r) for r in map_metric_ranges(j['requested'],j['captured'],j['ergs'])]
        if op=='kinds':return [api_kind(i) for i in range(65536)]
        if op=='frame_index':
            with Frame(j['frame']) as frame:return json.loads(json.dumps(build_metric_index(frame)))
        if op=='flags':return parse_query_flags(j['iterations'],j['descriptions'],j.get('initial',[]))
        if op=='times':return mi.update_metric_sample_times(j['previous'],j['parallel'])
        if op=='weight':return mi.choose_weight_metric(j['descriptions'])
        if op=='iteration_pass':return mi.iteration_pass(j['requested'],j.get('mapping',[]))
        if op=='values':return mi.prepare_iteration_values(j['iterations'],j['descriptions'],j['requested'],j['weights'],j.get('groups',[]),j.get('initial'))
        t=Transport(j['transport']);cancel=t.cancel if 'cancel_at' in j['transport'] else None
        try:
            r=mi.MetricIterationRunner(t);opts=copy.deepcopy(j.get('options',{}))
            if op=='outer':value=MetricOuterPassRunner(t).run(j['ids'],j['ranges'],j.get('requested',2**32-1),request_flag=j.get('request_flag',False),cancel=cancel,result=j.get('initial'))
            elif op=='execute':value=r.execute(j['ids'],j['ranges'],cancel=cancel,**opts)
            elif op=='collect':value=r.collect(j['ids'],j['ranges'],cancel=cancel,**opts)
            elif op=='execute_ranges':value=r.execute_ranges(j['ids'],j['requested_ranges'],j['captured'],j['ergs'],cancel=cancel,**opts);value['replay_ranges']=[list(x) for x in value['replay_ranges']]
            elif op=='execute_frame':
                with Frame(j['frame']) as frame:value=r.execute_frame(frame,j['ids'],j.get('requested_ranges'),cancel=cancel,**opts);value['replay_ranges']=[list(x) for x in value['replay_ranges']]
            else:raise AssertionError(op)
            result=dict(result=value)
        except mi.MetricIterationError as e:result=dict(error=str(e),partial=e.result)
        except (ValueError,RuntimeError) as e:result=dict(error=str(e))
        result['trace']=t.trace;return result
    def add(label,**j):
        j=copy.deepcopy(j);jobs.append(j);labels.append(label)
        try:expected.append(encode(reference(j)))
        except (ValueError,RuntimeError) as e:expected.append(dict(error=str(e)))
        return len(jobs)-1
    add('full uint16 classification',op='kinds')
    add('full static decoder inventory',op='decoder_types')
    for count,requested,flag in itertools.product([0,1,3,2**32-1],[0,1,2**32-1], [False,True]):add('pass selection',op='select',groups=count,requested=requested,flag=flag)
    for field,bad in [('groups',True),('groups',-1),('groups',2**32),('requested',True),('requested',2**64-1),('flag',0)]:
        j=dict(op='select',groups=1,requested=0,flag=False);j[field]=bad;add('invalid selector',**j)
    for categorized in [{},{'2':[]},{'2':[[2,0,1]]}]:add('range category lookup',op='range_table',categorized=categorized,fallback=[[9,3,4]])
    for i in range(200):add('range overlaps',op='ranges',requested=[[rng.randrange(3),rng.randrange(12),rng.randrange(12)] for _ in range(i%9)],captured=[[2,rng.randrange(12),rng.randrange(12)] for _ in range(i%11)],ergs=[11,4,99,42,8,77])
    for bad in [[2,True,3],[2,-1,3],[2,0,2**32],[2,3]]:add('invalid range',op='ranges',requested=[bad],captured=[],ergs=[])
    for previous,parallel in [([],[]),([[2**64-1,0]],[]),([[1,2]],[[]]),([],[[[3,4]],[[8,9]]]),([[True,1]],[]),([] ,[[[1,-1]]]),([],[[[1,2,3]]])]:add('sample times',op='times',previous=previous,parallel=parallel)
    for names in [[],*[[n] for n in mi.WEIGHT_NAMES],list(reversed(mi.WEIGHT_NAMES))]:add('weight precedence',op='weight',descriptions=[dict(name=n,id=i) for i,n in enumerate(names)])
    descs=[dict(id=7,name='intel.duration',kind=0),dict(id=42,name='value',kind=3),dict(id=43,name='other',kind=0)]
    flags=[dict(kind=1,label='ignored'),dict(kind=0,label='Z'),dict(kind=0,label='A'),dict(kind=0,label='Z')]
    for i in range(100):
        rows=[dict(metrics=[dict(metric=m,values=[1.]*3,aux=[rng.getrandbits(64) for _ in range(3)]) for m in [42,43]]) for _ in range(i%6)]
        initial=[dict(range=2,samples=[dict(iteration=0,index=17,flags=['seed','seed'])])]
        add('flag merges',op='flags',iterations=rows,descriptions=flags,initial=initial)
    for initial in [[dict(range=True,samples=[])],[dict(range=0,samples=[dict(iteration=True,index=0,flags=[])])],[dict(range=0,samples=[dict(iteration=0,index=-1,flags=[])])],[dict(range=0,samples=[dict(iteration=0,index=0,flags=[1])])]]:add('invalid initial flags',op='flags',iterations=[],descriptions=[],initial=initial)
    for count,selected,provided,timing,policy in itertools.product([0,1,3],[0,1,2**32-1],[False,True],['absent','empty','ordinary','multiple','boundary'],['normal','main_empty','weight_empty_first']):
        t=dict(descriptions=descs,timing=timing,policy=policy,flag_descriptions=flags)
        opts=dict(samples=count,requested_pass=selected,weights=[3.,7.] if provided else [],initial_sample_times=[[91,17]])
        add('iteration policy',op='execute',ids=[42,43],ranges=[[20,20],[30,30]],options=opts,transport=t)
    for cancel in ['always',0,1,2]:
        for op in ['outer','execute','collect']:add('cancellation '+op,op=op,ids=[42,43],ranges=[[20,20],[30,30]],transport=dict(descriptions=descs,cancel_at=cancel),options=dict(samples=3,weights=[3.,7.]))
    for flag in [False,True,1]:add('outer preparation flag',op='outer',ids=[42,43],ranges=[[20,20]],transport=dict(descriptions=descs,prepare_flag=flag))
    for fail in ['catalog','flag_catalog','prepare','replay']:add('transport exception',op='execute',ids=[42],ranges=[[1,1]],transport=dict(descriptions=descs,throw=fail),options=dict(weights=[1.]))
    add('direct bypass',op='execute',ids=[],ranges=[[1,1]],transport=dict(throw='catalog'),options=dict(requested_pass=True,samples=0,initial=[[{'untouched':1}]],initial_query_flags=[]))
    for bad in [True,-1,2**32]:add('invalid iteration count',op='execute',ids=[],ranges=[],transport={},options=dict(samples=bad))
    for mapping,requested in [([],0),([1,0],0),([1,0],2),([True],0),([2**32],0),([1],2**32-1)]:add('mapped pass',op='iteration_pass',requested=requested,mapping=mapping)
    for i in range(150):
        iterations=[dict(metrics=[dict(metric=mid,values=[rng.uniform(-10,20) for _ in range(4)]) for mid in [42,43,42]]) for _ in range(1+i%5)]
        add('position/duplicate aggregation',op='values',iterations=iterations,descriptions=descs,requested=[43,42,99],weights=[0.,1.,-2.,3.],groups=[0,1,0,1] if i%2 else [])
    add('boolean numeric conversion',op='values',iterations=[dict(metrics=[dict(metric=42,values=[True,False])])],descriptions=descs,requested=[42],weights=[True,False])

    flag_row=dict(metric=42,values=[1.,2.],aux=[0,1])
    for bad in [True,-1,2**64]:
        row=copy.deepcopy(flag_row);row['aux'][0]=bad
        add('invalid auxiliary mask',op='flags',iterations=[dict(metrics=[row])],descriptions=flags)
    for rows in [[dict(metrics=[flag_row]),dict(metrics=[])],
                 [dict(metrics=[flag_row,dict(metric=43,values=[1.],aux=[0])])],
                 [dict(metrics=[dict(metric=42,values=[1.,2.],aux=[0])])]]:
        add('invalid flag shape',op='flags',iterations=rows,descriptions=flags)
    for bad in [dict(kind=True,label='x'),dict(kind=2**32,label='x'),dict(kind=0,label=3)]:
        add('invalid descriptor',op='flags',iterations=[dict(metrics=[flag_row])],descriptions=[bad])
        add('empty descriptor bypass',op='flags',iterations=[],descriptions=[bad])
    add('extra auxiliary values ignored',op='flags',iterations=[dict(metrics=[dict(metric=42,values=[1.],aux=[0,True,-1])])],descriptions=flags)
    for initial in [[dict(range=0,samples=[])]*2,[dict(range=0,samples=[dict(iteration=0,index=0,flags=[])]*2)],
                    [dict(range=2**64,samples=[])],[dict(range=0,samples=[dict(iteration=2**64,index=0,flags=[])])]]:
        add('initial flag bounds and duplicates',op='flags',iterations=[],descriptions=[],initial=initial)
    for groups in [[],[0],[True,1],[-1,0],[0,2**31],[1.,1]]:
        for kind in [True,256,3]:
            add('group and kind validation order',op='values',iterations=[dict(metrics=[flag_row])],descriptions=[dict(id=42,kind=kind)],requested=[42],weights=[1.,1.],groups=groups)
    add('empty ranges before kind',op='values',iterations=[dict(metrics=[dict(metric=42,values=[])])],descriptions=[dict(id=42,kind=True)],requested=[42],weights=[])

    root=a.reference/'output/metric-scheduling'
    def load_oracle(name):
        path=root/name/'validation.json';d=read(path);assert d['passed'];sources.append(dict(path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),cases=len(d['cases'])))
        if 'binary' in d:assert hashlib.sha256(Path(d['binary']).read_bytes()).hexdigest()==d['sha256']
        return d['cases']
    for row in load_oracle('range-mapping-oracle-final'):
        captured=row['captured'] if row['mode']!='other' else [[17,0,0]]
        index=add('saved GPA range mapping',op='ranges',requested=row['requested'],captured=captured,ergs=row['ergs']);oracles.append((index,'direct',row['mapped']))
    for i,row in enumerate(load_oracle('outer-pass-oracle-verified')):
        plans,responses=wire_records(row['wire']);t=dict(plans=plans,responses=responses)
        if row['cancel_at'] is not None:t['cancel_at']=row['cancel_at']
        index=add('saved GPA outer receiver',op='outer',ids=[42,37,2**32-1],ranges=[[r*2,r*2+1] for r in range(row['ranges'])],requested=row['selected'],request_flag=bool(i%2),transport=t)
        oracles.append((index,'outer',dict(status=row['status'],result=row['result'])))
    for name in ['metric-iterations-oracle-aligned','iteration-failures-oracle','sample-times-oracle-final','query-flags-full-oracle']:
        for row in load_oracle(name):
            plans,responses=wire_records(row['wire']);t=dict(descriptions=row['descriptions'],plans=plans,responses=responses)
            if row.get('cancel_at') is not None:t['cancel_at']=row['cancel_at']
            opts=dict(samples=row['samples'],requested_pass=row['selected'],pass_mapping=row['mapping'],weights=row.get('supplied_weights',[3.,7.]) if row['provided'] else [])
            if name=='iteration-failures-oracle':opts['initial']=[[dict(values=[901.,902.],kind=6,weight=17.,median=123.,minimum=123.,maximum=123.,mean=123.,variation_percent=123.) for _ in range(2)] for _ in range(2)]
            if 'initial_times' in row:opts['initial_sample_times']=row['initial_times']
            if 'flag_catalog' in row:
                catalogs=[[],[dict(kind=1,label='ignored'),dict(kind=0,label='first warning'),dict(kind=0,label='second warning'),dict(kind=0,label='first warning')],[dict(kind=0 if i%2 else 3,label=f'flag-{i:03}-long label') for i in range(135)]]
                t['flag_descriptions']=catalogs[row['flag_catalog']]
                opts['initial_query_flags']=parse_query_flags([dict(metrics=[dict(metric=42,values=[1.]*4,aux=[1]*4)]) for _ in range(2)],[dict(kind=0,label='retained prior warning')]) if row['flag_seed'] else []
            index=add('saved GPA '+name,op='execute',ids=[42,43],ranges=[[20,20],[30,30]],options=opts,transport=t)
            fields=['selected_pass','iteration_count','weight_metric','weights','values']+ [k for k in ['status','complete','weight_attempts','iteration_statuses','sample_times','query_flags'] if k in row]
            oracles.append((index,'fields',{k:row[k] for k in fields}))
    for row in load_oracle('frame-index-oracle-final'):
        assert hashlib.sha256(Path(row['frame']).read_bytes()).hexdigest()==row['frame_sha256']
        assert hashlib.sha256(Path(row['oracle']).read_bytes()).hexdigest()==row['oracle_sha256']
        index=add('saved FrameFile index '+row['name'],op='frame_index',frame=row['frame']);oracles.append((index,'direct',row['result']))
    for name in ['GF2_Exilium_2026_03_03__00_19_35.gpa_frame','bf1_2026_01_21__16_53_05.gpa_frame']:
        path=a.reference/name;add('real frame index '+name,op='frame_index',frame=str(path))
        add('real frame direct replay ranges '+name,op='execute_frame',frame=str(path),ids=[],transport={})

    request,response=a.out/'requests.json',a.out/'native.json';request.write_text(json.dumps(wire(jobs),allow_nan=False),encoding='utf-8');(a.out/'expected.json').write_text(json.dumps(expected),encoding='utf-8')
    windows=Path(os.environ['SystemRoot']);env={k:v for k,v in os.environ.items() if k.upper() in {'SYSTEMROOT','SYSTEMDRIVE','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA'}};env['PATH']=str(a.qt_bin.resolve())+os.pathsep+str(windows/'System32')+os.pathsep+str(windows)
    subprocess.run([str(a.exe.resolve()),'--probe',str(request.resolve()),str(response.resolve())],check=True,env=env,timeout=240)
    actual=read(response);assert len(actual)==len(expected)
    failures=[dict(index=i,name=labels[i],expected=e,actual=g) for i,(g,e) in enumerate(zip(actual,expected)) if json.dumps(g,sort_keys=True)!=json.dumps(e,sort_keys=True)]
    cli_checks=[]
    if a.cli:
        for name in ['GF2_Exilium_2026_03_03__00_19_35.gpa_frame','bf1_2026_01_21__16_53_05.gpa_frame']:
            frame_path=(a.reference/name).resolve();target=(a.out/name).resolve()
            run=subprocess.run([str(a.cli.resolve()),'metric-index',str(frame_path),'--out',str(target)],env=env,capture_output=True,timeout=120)
            assert run.returncode==0,run.stderr.decode(errors='replace')
            actual_index=read(target/'metric-index.json')
            with Frame(frame_path) as frame:want=json.loads(json.dumps(build_metric_index(frame)))
            want.update(frame=str(frame_path),independent=True,category_scope=[2],all_framefile_categories=False)
            assert actual_index==want
            runtime=read(target/'report.json');assert runtime['completed']
            modules=[Path(x).name.lower() for x in runtime['loaded_modules']]
            assert not any(x.startswith(('python','gpa-','gpa_','tk8','tcl8')) or x=='renderdoc.dll' for x in modules),modules
            qt=[Path(x) for x in runtime['loaded_modules'] if Path(x).name.lower().startswith('qt6')]
            assert qt and all(x.parent.resolve()==a.qt_bin.resolve() for x in qt)
            cli_checks.append(dict(frame=name,internal_apis=len(want['ergs']),ranges=len(want['ranges']['2']),excluded=len(want['excluded']),passed=True))
    def close(g,w):
        if isinstance(g,dict) and '$f' in g:
            v=float('nan') if g['$f']=='nan' else struct.unpack('<d',bytes.fromhex(g['$f']))[0];w=float(w);return math.isnan(v) if math.isnan(w) else math.isclose(v,w,rel_tol=1e-14,abs_tol=1e-14)
        if isinstance(g,dict):return g.keys()==w.keys() and all(close(g[k],w[k]) for k in g)
        if isinstance(g,list):return len(g)==len(w) and all(close(x,y) for x,y in zip(g,w))
        return g==w
    for i,mode,want in oracles:
        got=actual[i] if mode=='direct' else actual[i].get('result',{})
        if mode=='fields':got={k:got.get(k) for k in want}
        if not close(got,want):failures.append(dict(index=i,name='direct oracle '+labels[i],actual=got,expected=want))
    report=dict(passed=not failures,cases=len(jobs),original_observations=len(oracles),uint16_classifications=65536,provenance=sources,failures=failures,cli_checks=cli_checks,
        exe_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),sources={name:hashlib.sha256((a.reference/'standalone'/name).read_bytes()).hexdigest() for name in ['metric_iterations.py','metric_outer_passes.py','metric_range_mapping.py','metric_query_flags.py','frame_metric_index.py','frame_api_types.json']})
    (a.out/'validation.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(dict(passed=report['passed'],cases=len(jobs),observations=len(oracles),failures=[dict(index=f['index'],name=f['name']) for f in failures[:15]]),indent=2))
    if failures:print(str(failures[0])[:6000]);return 1
    return 0


if __name__=='__main__':sys.exit(main())
