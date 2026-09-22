"""Native metric arithmetic, roster assembly and planning parity (development only)."""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import random
import struct
import subprocess
import sys


def encode(value):
    if type(value) is float:
        return {'$f': 'nan' if math.isnan(value) else struct.pack('<d', value).hex()}
    if isinstance(value, dict): return {k: encode(v) for k, v in value.items()}
    if isinstance(value, list): return [encode(v) for v in value]
    return value


def wire(value):
    if type(value) is float: return {'$f': struct.pack('<d', value).hex()}
    if isinstance(value, dict): return {k: wire(v) for k, v in value.items()}
    if isinstance(value, list): return [wire(v) for v in value]
    return value


def read(path): return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def fixture(mode='events', n=3):
    sets = [dict(name=name, metrics=[dict(name=m, label=m, unit='us', description=m,
            result_type=1, metric_type=0) for m in ['GpuTime', 'Active']]) for name in ['A', 'B']]
    ranges = [(7, None, None), (13, None, None)] if mode == 'events' else [(None, 2, 5)] if mode == 'interval' else [(None, 0, 4), (None, 7, 9)]
    records = []; passes = []
    for si, s in enumerate(sets):
        for i in range(n):
            pi = si*n+i
            passes.append(dict(pass_index=pi, set=s['name'], sample_index=i, image_matches=True, event_mapping_valid=True))
            for ri, (event, start, end) in enumerate(ranges):
                records.append(dict(set=s['name'], event=event, start_event=start, end_event=end, range_index=ri*3,
                    pass_index=pi, sample_index=i, available=True,
                    values=[dict(type=1, value=2**64-1 if i == 0 else 100+i), dict(type=2, value=float(i+ri+si))]))
    return dict(sample_count=n, sets=sets, records=records, selected_events=[7,13] if mode=='events' else [],
        selection_mode=mode, interval=dict(start_event=2,end_event=5), frame_ranges=dict(ranges=[dict(range_index=0,start_event=0,end_event=4),dict(range_index=3,start_event=7,end_event=9)]),
        sample_schedule='metric_set_then_iteration', validation=dict(passes=passes), frame_sha256='frame',experiment={},baseline_rgba_sha256='image')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference',type=Path,required=True); p.add_argument('--exe',type=Path,required=True)
    p.add_argument('--qt-bin',type=Path,required=True); p.add_argument('--out',type=Path,required=True)
    p.add_argument('--isolated-env',action='store_true'); a=p.parse_args(); a.out.mkdir(parents=True,exist_ok=False)
    sys.path.insert(0,str(a.reference/'standalone'))
    import metric_values as mv
    import metric_passes as mp
    import metric_planner as planner
    import md_publisher_values as publisher_values
    rng=random.Random(762231)
    jobs=[]; expected=[]; labels=[]; saved=[]; provenance=[]

    def reference(j):
        op=j['op']
        if op=='sum': return mv.sequential_sum(j['values'])
        if op=='stats': return mv.statistics(j['values'])
        if op=='construct': return mv.iterated(j['values'],j['kind'],j.get('weight',1))
        if op=='combine': return mv.combine(mv.iterated(**j['left']),mv.iterated(**j['right']))
        if op=='aggregate': return mv.aggregate_ranges(j['values'],j['kind'],j['weights'],j.get('groups'))
        if op=='summary': return mv.sample_summary(j['values'])
        if op=='assembly': return mp.assemble_iterations(j['iterations'])
        if op=='matrix': return mp.profile_matrix(j['profile'])
        if op=='summaries': return mv.summarize_records(j['profile'])
        if op=='groups': return planner.group_choices(j['choices'])
        if op=='plan': return planner.plan_metrics(j['catalog'],j['requested'])
        if op=='requested': return planner.requested_results(j['profile'],j['plan'])
        if op=='publisher_profile': return publisher_values.publisher_profile(j['profile'],j['publisher'])
        if op=='publisher_analysis': return publisher_values.publisher_analysis(j['profile'],j['publisher'])
        raise AssertionError(op)

    def add(label, **j):
        j=copy.deepcopy(j); jobs.append(j); labels.append(label)
        try: expected.append(encode(reference(j)))
        except ValueError as e: expected.append(dict(error=str(e)))
        return len(jobs)-1

    vectors=[[],[0.],[-0.],[0.,-0.],[-0.,0.],[1.,2.,3.],[-1.,-2.],[math.nan],[math.inf],[-math.inf],[math.inf,-math.inf],
             [1e308,1e308],[-1e308,1e308,1.],[5e-324,-5e-324],[1.,math.nan,3.],[float(2**64-1),0.]]
    for i,v in enumerate(vectors):
        for op in ['sum','stats','summary']: add(f'{op} boundary {i}',op=op,values=v)
        for kind in [0,1,2,3,4,5,6,255]:
            for weight in [0.,-0.,1.,-3.,1e308,math.inf,math.nan]:
                add(f'construct boundary {i}/{kind}/{weight}',op='construct',values=v,kind=kind,weight=weight)
    for i in range(300):
        v=[rng.uniform(-1e20,1e20) for _ in range(rng.randrange(1,35))]
        w=[rng.choice([0.,1.,-1.,rng.uniform(-100,100)]) for _ in range(2)]; kind=rng.randrange(256)
        add(f'random combine {i}',op='combine',left=dict(values=v,kind=kind,weight=w[0]),right=dict(values=v[::-1],kind=kind,weight=w[1]))
    for left,right in [([], [1.]),([1.],[]),([1.],[2.,3.]),([1.],[2.])]:
        for kinds in [(0,0),(0,3)]: add('empty/mismatched combine',op='combine',left=dict(values=left,kind=kinds[0]),right=dict(values=right,kind=kinds[1]))
    for bad in [-1,256,True,0.5,2**64-1]: add('invalid kind',op='construct',values=[1.],kind=bad)
    for i in range(250):
        count=rng.randrange(1,70); width=rng.randrange(0,10)
        add(f'aggregate {i}',op='aggregate',values=[[rng.uniform(-50,80) for _ in range(width)] for _ in range(count)],kind=rng.randrange(8),
            weights=[rng.choice([0.,1.,2.,-3.]) for _ in range(count)],groups=[] if i%3==0 else [rng.randrange(7) for _ in range(count)])
    for changes in [dict(values=[] ,weights=[]),dict(weights=[]),dict(groups=[-1]),dict(groups=[True]),dict(groups=[2**31]),dict(groups=[1,2])]:
        j=dict(op='aggregate',values=[[1.]],weights=[1.],kind=3);j.update(changes);add('aggregate rejection',**j)
    add('summary publication',op='summary',values=[None,True,False,'1',{},math.nan,math.inf,0.,1.,2**64-1])

    for mode in ['events','interval','frame_ranges']:
        profile=fixture(mode)
        for op in ['matrix','summaries']: add(mode+' original',op=op,profile=profile)
        changed=copy.deepcopy(profile);rng.shuffle(changed['records']);rng.shuffle(changed['validation']['passes'])
        for op in ['matrix','summaries']: add(mode+' serialization order',op=op,profile=changed)
        for value in [None,True,False,math.nan,math.inf,-math.inf,0.,-0.,2**64-1]:
            changed=copy.deepcopy(profile);changed['records'][0]['values'][0]['value']=value
            for op in ['matrix','summaries']: add(mode+' publication values',op=op,profile=changed)
        mutations=[lambda p:p['records'].pop(),lambda p:p['records'].append(copy.deepcopy(p['records'][0])),
            lambda p:p.update(records=[r for r in p['records'] if r['set']!='B']),
            lambda p:p.update(records=[r for r in p['records'] if r['sample_index']!=1]),
            lambda p:p['validation']['passes'].pop(),lambda p:p.update(sample_schedule='bad'),
            lambda p:p.update(sets=p['sets']+p['sets']),lambda p:p['sets'][0].update(metrics=[]),
            lambda p:p['sets'][0]['metrics'].append(p['sets'][0]['metrics'][0]),lambda p:p['records'][0]['values'].pop()]
        for field,values in [('sample_count',[0,101,True,1.5,2**64-1]),('selected_events',[[True],[1,1]]),('selection_mode',['bad'])]:
            for value in values: mutations.append(lambda p,f=field,v=value:p.update({f:v}))
        for field,values in [('pass_index',[-1,True,100,1]),('sample_index',[True,-1,2]),('available',[0,None]),('set',['missing'])]:
            for value in values: mutations.append(lambda p,f=field,v=value:p['records'][0].update({f:v}))
        for field,values in [('pass_index',[True,-1,1]),('sample_index',[True,2]),('image_matches',[False,1]),('event_mapping_valid',[False,1])]:
            for value in values: mutations.append(lambda p,f=field,v=value:p['validation']['passes'][0].update({f:v}))
        if mode=='events':
            mutations.extend([lambda p:p.update(selected_events=[]),lambda p:p['records'][0].update(event=True),lambda p:p['records'][0].update(start_event=0)])
        if mode=='interval':
            mutations.extend([lambda p:p['interval'].update(start_event=True),lambda p:p['interval'].update(start_event=10),lambda p:p['records'][0].update(end_event=2.5)])
        if mode=='frame_ranges':
            mutations.extend([lambda p:p['frame_ranges']['ranges'].clear(),lambda p:p['frame_ranges']['ranges'][1].update(start_event=4),
                lambda p:p['frame_ranges']['ranges'][1].update(range_index=0),lambda p:p['frame_ranges']['ranges'][0].update(range_index=-1),
                lambda p:p['frame_ranges']['ranges'][0].update(start_event=True),lambda p:p['frame_ranges']['ranges'][0].update(start_event=8),
                lambda p:p['records'][0].update(range_index=True),lambda p:p['records'][0].update(range_index=20),lambda p:p['records'][0].update(end_event=20),
                lambda p:p['records'][0].update(start_event=False,end_event=4.)])
        for i,mutate in enumerate(mutations):
            changed=copy.deepcopy(profile);mutate(changed);add(f'{mode} identity {i}',op='matrix',profile=changed)
        plan=dict(passes=[dict(set='A',metrics=['GpuTime']),dict(set='B',metrics=['Active'])],requested_metrics=['Active','GpuTime'])
        if mode=='frame_ranges':
            changed=copy.deepcopy(profile);changed['records'][0].update(start_event=False,end_event=4.)
            add('requested accepted numeric boundary aliases',op='requested',profile=changed,plan=plan)
        for value in [None,False,True,2**64-1,math.inf]:
            changed=copy.deepcopy(profile);changed['records'][0]['values'][0]['value']=value
            rng.shuffle(changed['records']);add('requested exact values '+mode,op='requested',profile=changed,plan=plan)
        for change in [lambda p:p['passes'][0].update(set='B'),lambda p:p['passes'][0]['metrics'].append('Active'),lambda p:p.update(requested_metrics=['Missing','Active'])]:
            changed=copy.deepcopy(plan);change(changed);add('requested assignment rejection',op='requested',profile=profile,plan=changed)
        absent=copy.deepcopy(plan);absent['passes'][0]['metrics']=['Missing'];absent['requested_metrics']=['Missing','Active']
        add('requested absent descriptor',op='requested',profile=profile,plan=absent)
        profile['metric_request']=plan
        publisher=dict(records=copy.deepcopy(profile['records']))
        for row in publisher['records']:
            row['values'][0]['value']=float(row['sample_index'])
        before=copy.deepcopy(profile)
        for op in ['publisher_profile','publisher_analysis']:add('publisher conversion '+mode,op=op,profile=profile,publisher=publisher)
        assert profile==before
        for mutate in [lambda p:p['records'].pop(),lambda p:p['records'].append(copy.deepcopy(p['records'][-1])),
                lambda p:p['records'].reverse(),lambda p:p['records'][0].update(raw_sha256='bad'),
                lambda p:p['records'][0]['values'].pop(),lambda p:p['records'][0].update(available=False)]:
            changed=copy.deepcopy(publisher);mutate(changed);add('publisher identity '+mode,op='publisher_analysis',profile=profile,publisher=changed)
        profile['records'][0]['available']=False
        add('publisher retains raw availability',op='publisher_analysis',profile=profile,publisher=publisher)

    for start,end in [(-1,2**64-1),(0,2**64-1),(2**53+1,2**53+2),(2**64-1,2**64-1)]:
        profile=fixture('frame_ranges',1)
        profile['frame_ranges']['ranges']=[dict(range_index=0,start_event=start,end_event=end)]
        profile['records']=[r for r in profile['records'] if r['range_index']==0]
        for r in profile['records']:r.update(start_event=start,end_event=end)
        add('large integer boundaries',op='matrix',profile=profile)
        changed=copy.deepcopy(profile);changed['records'][0]['start_event']=float(start)
        add('exact integer float equality',op='matrix',profile=changed)
    for i in range(250):
        iterations=[[dict(metric=m,values=[rng.random()*1e9 for _ in range(i%7)]) for m in [2**64-1,0,2**53+1]] for _ in range(1+i%5)]
        add('assembly generated',op='assembly',iterations=iterations)
    for iterations in [[],[[]],[[dict(metric=True,values=[])]],[[dict(metric=-1,values=[])]],
            [[dict(metric=1,values=[]),dict(metric=1,values=[])]],[[dict(metric=1,values=[True])]],
            [[dict(metric=1,values=[])],[dict(metric=1.,values=[])]],
            [[dict(metric=1,values=[])],[dict(metric=2,values=[])]],
            [[dict(metric=1,values=[1])],[dict(metric=1,values=[])]]]:add('assembly rejection',op='assembly',iterations=iterations)
    for i in range(500):
        add('ordered group generated',op='groups',choices=[[rng.randrange(-5,8) for _ in range(rng.randrange(0,7))] for _ in range(i%25)])
    for choices in [[[2],[1],[1,2]],[[],[1],[2]],[[True]],[[2**31]],[[-2**31-1]],[[1.]],[[2**64-1]],[[0],[-2**31],[2**31-1]]]:
        add('ordered group boundary',op='groups',choices=choices)
    catalog=dict(sets=fixture()['sets'])
    for requested in [['GpuTime','Active'],['Active','GpuTime'],[],['Missing'],['Active','Active'],[None],['']]:add('plan request',op='plan',catalog=catalog,requested=requested)
    for mutate in [lambda c:c['sets'].append(c['sets'][0]),lambda c:c['sets'][1]['metrics'][0].update(unit='bad'),lambda c:c['sets'][0]['metrics'].append(c['sets'][0]['metrics'][0])]:
        changed=copy.deepcopy(catalog);mutate(changed);add('plan definitions',op='plan',catalog=changed,requested=['GpuTime'])

    root=a.reference/'output/metric-scheduling'
    for file,typ in [('oracle-v2/oracle.json','numeric'),('assembly-oracle-live/oracle.json','assembly'),('group-planner-catalog-oracle/oracle.json','groups')]:
        path=root/file; oracle=read(path)
        assert hashlib.sha256(Path(oracle['binary']).read_bytes()).hexdigest()==oracle['sha256'],file
        provenance.append(dict(path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),rows=len(oracle['rows']),binary_sha256=oracle['sha256']))
        for i,row in enumerate(oracle['rows']):
            if typ=='numeric':
                if row['operation']=='construct': j=dict(op='construct',values=list(map(float,row['values'])),kind=row['kind'],weight=row['weight'])
                else:j=dict(op='combine',left=dict(values=list(map(float,row['left'])),kind=row['kind'],weight=row['weights'][0]),right=dict(values=list(map(float,row['right'])),kind=row['kind'],weight=row['weights'][1]))
            elif typ=='assembly':j=dict(op='assembly',iterations=[[dict(metric=m['metric'],values=list(map(float,m['values']))) for m in it] for it in row['iterations']])
            else:j=dict(op='groups',choices=row['choices'])
            index=add(f'saved GPA {typ} {i}',**j);saved.append((index,typ,row))
    catalog=read(root/'descriptor-catalog/catalog.json')
    provenance.append(dict(path=str(root/'descriptor-catalog/catalog.json'),sha256=hashlib.sha256((root/'descriptor-catalog/catalog.json').read_bytes()).hexdigest()))
    add('real catalog planner',op='plan',catalog=catalog,requested=['GpuTime','EuActive','Sampler00InputAvailable','Sampler00OutputReady'])
    paths=[root/'hotspots-gf2-v2/metrics/profile.json',root/'hotspots-gf2-v2/weights/profile.json']
    paths += [Path(p.read_text().strip()) for p in (root/'publisher-statistics-desktop').glob('*profile-path.txt')]
    for path in paths:
        profile=read(path);provenance.append(dict(path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),records=len(profile['records'])))
        for op in ['matrix','summaries']:add('saved profile '+str(path),op=op,profile=profile)
        if 'metric_request' in profile:add('saved requested profile',op='requested',profile=profile,plan=profile['metric_request'])
        sidecar=path.parent/'publisher-values.json'
        if sidecar.exists():
            publisher=read(sidecar)
            provenance.append(dict(path=str(sidecar),sha256=hashlib.sha256(sidecar.read_bytes()).hexdigest()))
            for op in ['publisher_profile','publisher_analysis']:add('saved publisher '+str(path),op=op,profile=profile,publisher=publisher)

    request=a.out/'requests.json';response=a.out/'native.json'
    request.write_text(json.dumps(wire(jobs),allow_nan=False),encoding='utf-8')
    (a.out/'expected.json').write_text(json.dumps(expected,indent=2),encoding='utf-8')
    env=os.environ.copy();system_root=env.get('SystemRoot',env.get('SYSTEMROOT','C:/Windows'))
    if a.isolated_env:
        env={k:v for k,v in env.items() if k.upper() in {'SYSTEMROOT','SYSTEMDRIVE','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}}
    env['PATH']=str(a.qt_bin)+os.pathsep+(str(Path(system_root)/'System32')+os.pathsep+system_root if a.isolated_env else env['PATH'])
    subprocess.run([str(a.exe.resolve()),'--probe',str(request.resolve()),str(response.resolve())],env=env,check=True,timeout=180)
    actual=read(response);checks=[]
    assert len(actual)==len(expected)
    for i,(got,want) in enumerate(zip(actual,expected)):
        checks.append(dict(index=i,name=labels[i],passed=json.dumps(got,sort_keys=True)==json.dumps(want,sort_keys=True)))
    def numeric_close(got,want,exact=False):
        if isinstance(got,dict) and '$f' in got:
            g=float('nan') if got['$f']=='nan' else struct.unpack('<d',bytes.fromhex(got['$f']))[0];w=float(want)
            if math.isnan(w):return math.isnan(g)
            return struct.pack('<d',g)==struct.pack('<d',w) if exact else math.isclose(g,w,rel_tol=1e-14,abs_tol=1e-14)
        if isinstance(got,dict):return got.keys()==want.keys() and all(numeric_close(got[k],want[k],exact) for k in got)
        if isinstance(got,list):return len(got)==len(want) and all(numeric_close(g,w,exact) for g,w in zip(got,want))
        return got==want
    for index,typ,row in saved:
        got=actual[index]
        okay=([g['metrics'] for g in got]==row['passes']) if typ=='groups' and 'error' not in got else numeric_close(got,row['result'],typ=='assembly')
        checks.append(dict(index=index,name='direct oracle '+labels[index],passed=okay))
    report=dict(passed=all(c['passed'] for c in checks),cases=len(jobs),checks=checks,provenance=provenance,
        by_operation={op:sum(j['op']==op for j in jobs) for op in sorted({j['op'] for j in jobs})},
        comparison='Exact binary64 bits against Python, NaNs classified; saved GPA arithmetic tolerance 1e-14, assembly exact bits, planner exact pass assignments.',
        executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),
        sources={name:hashlib.sha256((a.reference/'standalone'/name).read_bytes()).hexdigest() for name in ['metric_values.py','metric_passes.py','metric_planner.py','md_publisher_values.py']})
    (a.out/'validation.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    failures=[c for c in checks if not c['passed']]
    print(json.dumps(dict(passed=report['passed'],cases=len(jobs),checks=len(checks),failures=failures[:15]),indent=2))
    if failures:
        i=failures[0]['index'];print('FIRST',json.dumps(dict(job=wire(jobs[i]),actual=actual[i],expected=expected[i]))[:6000]);return 1
    return 0


if __name__=='__main__':sys.exit(main())
