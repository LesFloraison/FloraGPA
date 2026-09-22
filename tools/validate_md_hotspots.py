"""Development-only comparison of native event groups with the original Python collector."""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


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


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('reference','exe','probe','out'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--qt-bin',type=Path)
    parser.add_argument('--offline-only',action='store_true')
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    sys.path.insert(0,str(args.reference/'standalone'))
    import md_hotspots as original
    from metric_kinds import descriptor_kind
    from validate_metric_analysis import fixture
    env=os.environ.copy()
    system=os.environ['SYSTEMROOT']
    env['PATH']=os.pathsep.join(filter(None,[str(args.qt_bin) if args.qt_bin else '',os.path.join(system,'System32'),system]))
    def profile(n=3,weight=False):
        p=fixture('events',n)
        p.update(baseline_matches=True,adapter_luid=[17,0],catalog_version=[1,14,0],provenance={'bridge':'same'})
        if weight:
            p['sets']=p['sets'][:1];p['records']=[r for r in p['records'] if r['set']=='A'];p['validation']['passes']=p['validation']['passes'][:n]
        for s in p['sets']:
            for d in s['metrics']:
                if d['name']=='Active':d.update(metric_type=6,unit='percent')
                d['gpa_kind']=descriptor_kind(d['metric_type'],d['unit'],d['name'])
        return p
    base=profile();weights=profile(1,True)
    cases=[]
    def add(name,op='aggregate',**kw):cases.append((name,dict(op=op,**kw)))
    def pair(name,edit=None,groups=None):
        p,w=copy.deepcopy(base),copy.deepcopy(weights)
        if edit:edit(p,w)
        add(name,profile=p,weights=w,groups=groups)
    pair('default');pair('overlap',groups=[dict(name='B, "two"',events=[13,7]),dict(name='unicode 分组',events=[7])])
    for groups in ([],{},[None],[{}],[dict(name='',events=[7])],[dict(name='\u001c',events=[7])],[dict(name='A',events=[])],[dict(name='A',events=[7,7])],[dict(name='A',events=[True])],[dict(name='A',events=[7.0])],[dict(name='A',events=[9])],[dict(name='A',events=[7]),dict(name='A',events=[13])]):
        add('normalize-domain',op='normalize',groups=groups,events=[7,13])
    for ids in ([13,7],[-1,2**64-1],[1],[]):
        add('normalize-order',op='normalize',groups=None,events=ids)
    for value in (None,False,-1,0,1,2**64-1,float('nan'),float('inf'),1e-300):
        pair('weight-value',lambda p,w,v=value:w['records'][0]['values'][0].update(value=v))
    for value in (None,False,-1,0,1,2**64-1,float('nan'),float('inf'),1e308,1e-300):
        pair('metric-value',lambda p,w,v=value:p['records'][0]['values'][0].update(value=v))
    for typ in range(8):
        for unit in ('ticks','percent'):
            def edit(p,w,typ=typ,unit=unit):
                for s in p['sets']:
                    s['metrics'][1].update(metric_type=typ,unit=unit,gpa_kind=descriptor_kind(typ,unit,'Active'))
            pair('descriptor-kind',edit)
    for key in ('frame_sha256','experiment','baseline_rgba_sha256','adapter_luid','catalog_version','provenance'):
        pair('identity-'+key,lambda p,w,k=key:w.update({k:'different'}))
    for side in ('profile','weights'):
        for key,value in [('selection_mode','interval'),('baseline_matches',False),('sample_count',True),('sample_count',0),('sample_count',101),('selected_events',[7,7])]:
            p,w=copy.deepcopy(base),copy.deepcopy(weights);(p if side=='profile' else w)[key]=value;add('roster-'+key,profile=p,weights=w)
        for action in ('missing','duplicate','bad-event','float-event','bad-size','false-available'):
            p,w=copy.deepcopy(base),copy.deepcopy(weights);target=p if side=='profile' else w
            if action=='missing':target['records'].pop()
            elif action=='duplicate':target['records'].append(copy.deepcopy(target['records'][0]))
            elif action=='bad-event':target['records'][0]['event']=99
            elif action=='float-event':target['records'][0]['event']=7.
            elif action=='bad-size':target['records'][0]['values'].pop()
            else:target['records'][0]['available']=False
            add('record-'+action,profile=p,weights=w)
    rng=random.Random(9137)
    for i in range(180):
        p,w=profile(rng.randrange(1,5)),profile(1,True)
        for r in p['records']:
            r['available']=rng.random()>.1
            for v in r['values']:v['value']=rng.choice([0.,1.,-1.,1e100,1e-100,rng.random()*1e8,None])
        for r in w['records']:r['values'][0]['value']=rng.choice([0.,1.,10.,1e-100,None])
        add('random-'+str(i),profile=p,weights=w,groups=[dict(name='both',events=[13,7]),dict(name='one',events=[13])])
    def publisher(p,scale):
        rows=copy.deepcopy(p['records'])
        for r in rows:
            for v in r['values']:
                v['value']=float(v['value'])*scale if type(v['value']) in (int,float) else None
        return dict(records=rows)
    for scale in (0.,1e-3,2.):
        add('publisher',op='publisher',profile=base,weights=weights,publisher=publisher(base,scale),weight_publisher=publisher(weights,scale),groups=None)
    result=original.aggregate_profiles(base,weights,[dict(name='quoted, "group"',events=[7,13])]);add('csv',op='csv',result=result)
    def execute(job):
        if job['op']=='normalize':return original.normalize_groups(job['groups'],job['events'])
        if job['op']=='aggregate':return original.aggregate_profiles(job['profile'],job['weights'],job.get('groups'))
        if job['op']=='publisher':return original.aggregate_publisher_profiles(job['profile'],job['weights'],job['publisher'],job['weight_publisher'],job.get('groups'))
        if job['op']=='load':return original.load_publisher_aggregates(Path(job['directory']),job['result'])
        if job['op']=='csv':
            path=args.out/'expected.csv';original.write_csv(path,job['result']);return path.read_bytes().decode('utf-8')
        raise ValueError(job['op'])
    def compare(batch,stem):
        incoming=args.out/(stem+'-input.json');outgoing=args.out/(stem+'-output.json')
        incoming.write_text(json.dumps(wire([j for _,j in batch]),ensure_ascii=False),encoding='utf-8')
        subprocess.run([str(args.probe.resolve()),'--probe',str(incoming.resolve()),str(outgoing.resolve())],env=env,check=True)
        actual=read(outgoing);assert len(actual)==len(batch)
        checked=[]
        for (name,job),native in zip(batch,actual,strict=True):
            try:expected=encode(execute(job))
            except (ValueError,KeyError,TypeError,IndexError,OverflowError):expected={'error':True}
            equal=('error' in native if isinstance(expected,dict) and 'error' in expected else native==expected)
            checked.append(dict(name=name,passed=equal,rejected=isinstance(expected,dict) and 'error' in expected))
            if not equal:
                (args.out/(stem+'-mismatch.json')).write_text(json.dumps(dict(name=name,expected=expected,actual=native),ensure_ascii=False,indent=2),encoding='utf-8')
                raise AssertionError('Original/native mismatch: '+name)
        return checked
    checks=compare(cases,'arithmetic')
    collections=[];owner_checks=0;module_reports=[]
    if not args.offline_only:
        capture=args.reference/'GF2_Exilium_2026_03_03__00_19_35.gpa_frame'
        experiment_file=args.out/'input-experiment.json'
        experiment_file.write_text(json.dumps(dict(format='FloraGPA experiment 1',frame_sha256=hashlib.sha256(capture.read_bytes()).hexdigest(),frame_name=capture.name,cursor=1,history=[dict(label='Disable first draw',operations=[dict(kind='enabled',event=113,value=False)])])),encoding='utf-8')
        for name,extra,groups in [
            ('default',['--event','113','--event','181','--samples','2'],None),
            ('overlap-publisher',['--event','113','--event','181','--samples','2','--publisher-values'],[dict(name='pair',events=[181,113]),dict(name='first',events=[113])]),
            ('sets',['--event','113','--samples','1','--set','RenderBasic','--set','ComputeBasic'],None),
            ('experiment',['--event','181','--samples','1','--publisher-values','--experiment',str(experiment_file.resolve())],[dict(name='edited',events=[181])]),
        ]:
            folder=args.out/name;cmd=[str(args.exe.resolve()),'metric-groups',str(capture),'--out',str(folder.resolve()),'--warmup','0',*extra]
            if groups is not None:
                group_file=args.out/(name+'-groups.json');group_file.write_text(json.dumps(groups),encoding='utf-8');cmd+=['--groups',str(group_file.resolve())]
            completed=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=300);(args.out/(name+'.log')).write_text(completed.stdout+completed.stderr,encoding='utf-8');assert completed.returncode==0,completed.stderr
            result=read(folder/'aggregates.json');p=read(folder/'metrics/profile.json');w=read(folder/'weights/profile.json')
            expected=original.aggregate_profiles(p,w,groups);expected['sources']=result['sources'];
            if '--publisher-values' in extra:expected['publisher_aggregates']='publisher-aggregates.json'
            assert result==expected
            for phase in ('weights','metrics'):
                path=folder/phase/'profile.json';assert result['sources'][phase]==dict(path=f'{phase}/profile.json',sha256=hashlib.sha256(path.read_bytes()).hexdigest())
                assert read(folder/phase/'priority-audit.json')['lock']['closed'] is True
            for path in folder.rglob('*.json'):
                value=read(path)
                if not isinstance(value,dict) or 'loaded_modules' not in value:continue
                loaded=[Path(p) for p in value['loaded_modules']];assert loaded
                assert not any(p.name.lower().startswith(('python','gpa-','gpa_','tk8','tcl8')) or p.name.lower()=='renderdoc.dll' for p in loaded)
                module_reports.append(str(path.relative_to(args.out)))
            csv_path=args.out/(name+'-expected.csv');original.write_csv(csv_path,result);assert csv_path.read_bytes()==(folder/'aggregates.csv').read_bytes()
            if '--publisher-values' in extra:
                converted=original.load_publisher_aggregates(folder,result);original.write_csv(csv_path,converted);assert csv_path.read_bytes()==(folder/'publisher-aggregates.csv').read_bytes()
                checks+=compare([(name+'-reader',dict(op='load',directory=str(folder.resolve()),result=result))],name+'-reader')
                corrupt=args.out/(name+'-corrupted');shutil.copytree(folder,corrupt);saved=read(corrupt/'publisher-aggregates.json');saved['records'][0]['median']=987654321.;(corrupt/'publisher-aggregates.json').write_text(json.dumps(saved),encoding='utf-8')
                checks+=compare([('tampered-aggregate',dict(op='load',directory=str(corrupt.resolve()),result=result))],name+'-tampered-reader')
                mutations=[]
                for key in ('publisher_aggregates','frame_sha256','experiment','baseline_rgba_sha256','sample_count','groups'):
                    bad=copy.deepcopy(result);bad[key]='wrong';mutations.append(('reader-'+key,dict(op='load',directory=str(folder.resolve()),result=bad)))
                for phase in ('weights','metrics'):
                    for key in ('path','sha256'):
                        bad=copy.deepcopy(result);bad['sources'][phase][key]='wrong';mutations.append(('reader-source-'+phase+'-'+key,dict(op='load',directory=str(folder.resolve()),result=bad)))
                checks+=compare(mutations,name+'-reader-domain')
            collections.append(dict(name=name,raw_reports=len(p['records'])+len(w['records']),records=len(result['records']),publisher='--publisher-values' in extra))
        original_result=original.collect(capture,args.out/'python-owner',events=(113,181),samples=2,warmup=0,groups=[dict(name='pair',events=[181,113]),dict(name='first',events=[113])],publisher_values=True,bridge=args.exe.resolve().parent/'FloraGPA.Metrics.dll')
        native=read(args.out/'overlap-publisher/aggregates.json')
        for key in ('schema_version','mode','frame_sha256','experiment','baseline_rgba_sha256','groups','sample_count','ordering','semantics','limits'):
            assert original_result[key]==native[key],key;owner_checks+=1
        for key in ('set','metric','unit','samples'):
            assert original_result['weight'][key]==native['weight'][key],key;owner_checks+=1
        for a,b in zip(original_result['records'],native['records'],strict=True):
            for key in ('group','set','metric','label','unit','gpa_kind','events','total_samples'):
                assert a[key]==b[key],key;owner_checks+=1
        for phase in ('weights','metrics'):
            p=read(args.out/'python-owner'/phase/'profile.json');q=read(args.out/'overlap-publisher'/phase/'profile.json')
            for key in ('selected_events','sample_count','warmup_count','sets','adapter_luid','catalog_version','baseline_rgba_sha256'):
                assert p[key]==q[key],key;owner_checks+=1
    result=dict(passed=True,checks=len(checks),rejected=sum(c['rejected'] for c in checks),collections=collections,original_owner_checks=owner_checks,native_module_reports=module_reports,cases=checks)
    (args.out/'validation.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps({k:v for k,v in result.items() if k!='cases'},indent=2))


if __name__=='__main__':main()
