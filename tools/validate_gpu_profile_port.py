"""Native repeated timestamp profiling versus the preserved Python implementation.

Durations are independent GPU acquisitions. Compare each report's arithmetic
against its own ticks, and compare storage/selection/provenance across engines.
"""
import argparse
import base64
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import random
import subprocess
import sys


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--reference',type=Path,required=True)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--qt-bin',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--frames',type=Path)
    args=parser.parse_args();args.out=args.out.resolve();args.out.mkdir(parents=True,exist_ok=False)
    sys.path[:0]=[str(args.reference/'standalone'),str(args.reference/'tools')]
    import gpu_profile
    from frame import Frame
    from engine import Engine
    from devices import create_device
    from validate_statistics_ranges import range_fixture
    from validate_buffer_edits import experiment
    from validate_predication import fixture as predicate_fixture
    from validate_so_passthrough import multi_fixture
    from validate_so_counts import fixture as auto_fixture
    jobs=[];expected=[]
    def add(job,value):jobs.append(job);expected.append(value)
    for values in [[],[2],[1,2,3,4],[.1,.2,.3],[1e10,1e-8,1e-9], [1e10]*3]:
        add(dict(action='distribution',values=values),gpu_profile.distribution(values))
    rng=random.Random(731)
    for _ in range(150):
        values=[rng.random()*10**rng.randint(-9,6) for _ in range(rng.randint(0,80))]
        add(dict(action='distribution',values=values),gpu_profile.distribution(values))
    for freq in [0,1000,1000000000]:
        for disjoint in [False,True]:
            for start,end,origin,limit in [(1,2,0,3),(2,1,0,3),(0,2,1,3),(1,4,0,3),(1,1,0,3),(2**60,2**60+99,2**60-5,2**60+100)]:
                add(dict(action='timing',values=[freq,disjoint,start,end,origin,limit]),gpu_profile.timing(freq,disjoint,start,end,origin,limit))
    capture=range_fixture(args.out/'range.gpa_frame')
    with Frame(capture) as frame:
        for first,last,writes in [(None,None,False),(None,None,True),(150,160,True),(150,160,False),(100,200,False),(200,100,True),(99,200,False),(True,None,False)]:
            request=dict(start=first,end=last,include_writes=writes)
            try:
                a,b,entries=gpu_profile.selection(frame,first,last,writes)
                value=dict(start=a,end=b,events=[e.id for e in entries])
            except ValueError:value={'error':True}
            add(dict(action='selection',capture=str(capture),request=request),value)
        for request in [dict(samples=0),dict(samples=1001),dict(samples=True),dict(warmup=-1),dict(include_writes=1),dict(start=1.5)]:
            add(dict(action='selection',capture=str(capture),request=request),{'error':True})

    gpu=[]
    def fixture_case(name,path,warp=False,request=None,buffers=(),textures=(),disabled=(),output=False,operations=(),shaders=None):
        request=request or dict(samples=3,warmup=1,include_writes=True)
        job=dict(action='profile',capture=str(path),warp=warp,request=request,buffers=list(buffers),textures=list(textures),output=output,out=str(args.out/(name+'-native')))
        with Frame(path) as frame:
            dev=create_device('warp' if warp else 'hardware')
            try:
                exp=None
                ops=list(operations)+[dict(kind='enabled',event=id,value=False) for id in disabled]
                if shaders:
                    for id,raw in shaders.items():
                        ops.append(dict(kind='shader',resource=id,asset=dict(data=base64.b64encode(raw).decode('ascii'),sha256=hashlib.sha256(raw).hexdigest())))
                if ops:
                    exp_path=args.out/(name+'-experiment.json');exp=experiment(frame,exp_path,ops);job['experiment']=str(exp_path)
                engine=Engine(frame,dev,experiment=exp)
                report=gpu_profile.profile(engine,args.out/(name+'-reference'),**request)
                storage={str(id):hashlib.sha256(engine.read_buffer(id)).hexdigest() for id in buffers}
                storage.update({str(id):hashlib.sha256(engine.read_texture(id)).hexdigest() for id in textures})
                if output:
                    _,_,pixels=engine.readback(engine.last_target);storage['output']=hashlib.sha256(pixels).hexdigest()
                report['test_storage']=storage
                report['test_draw_auto']={str(id):value['parameters']['vertex_count'] for id,value in engine.so_auto_results.items()}
            finally:dev.close()
        # Match the exported JSON contract: resource/event object keys become
        # strings in both implementations, including nonempty shader histories.
        report=json.loads(json.dumps(report))
        gpu.append((name,len(jobs)));add(job,report)
        return report
    for warp in [False,True]:
        suffix='warp' if warp else 'hardware'
        fixture_case('range-'+suffix,capture,warp,buffers=[28],textures=[30])
        fixture_case('subset-'+suffix,capture,warp,dict(start=150,end=160,samples=2,warmup=0,include_writes=True),buffers=[28],textures=[30])
        fixture_case('disabled-'+suffix,capture,warp,buffers=[28],textures=[30],disabled=[200])
        fixture_case('disabled-write-'+suffix,capture,warp,buffers=[28],textures=[30],disabled=[100,160])
        for visible,value in [(False,0),(True,0),(True,1)]:
            path=args.out/f'predicate-{visible}-{value}-{suffix}.gpa_frame';predicate_fixture(path,visible,value)
            fixture_case(path.stem,path,warp,buffers=[82],textures=[30])
        path=args.out/('so-'+suffix+'.gpa_frame');multi_fixture(path,0)
        fixture_case(path.stem,path,warp,buffers=[40,42,44,46],textures=[50])
        path=args.out/('auto-'+suffix+'.gpa_frame');ops,shaders=auto_fixture(path,'doubled')
        report=fixture_case(path.stem,path,warp,buffers=[40],textures=[50],operations=ops,shaders=shaders)
        assert report['test_draw_auto']=={'200':12}
    if args.frames:
        for name in ['GF2_Exilium_2026_03_03__00_19_35.gpa_frame','bf1_2026_01_21__16_53_05.gpa_frame']:
            path=(args.frames/name).resolve()
            for writes in [False,True]:fixture_case(name+('-writes' if writes else ''),path,request=dict(samples=2,warmup=1,include_writes=writes),output=True)
            event=1455 if name.startswith('GF2') else 876
            fixture_case(name+'-disabled',path,request=dict(samples=2,warmup=0,include_writes=True),disabled=[event],output=True)
            report=fixture_case(name+'-undo',path,request=dict(samples=1,warmup=0,include_writes=False),output=True)
            assert report['test_storage']['output']==('2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1' if name.startswith('GF2') else '1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6')
    path=args.out/'jobs.json';path.write_text(json.dumps(jobs),'utf-8')
    env=dict(os.environ);env['QT_QPA_PLATFORM']='offscreen';env['PATH']=str(args.qt_bin.resolve())+os.pathsep+str(Path(os.environ['SystemRoot'])/'System32')
    run=subprocess.run([str(args.probe.resolve()),'--probe',str(path),str(args.out/'native.json')],env=env,capture_output=True,timeout=300)
    (args.out/'probe.log').write_bytes(run.stdout+run.stderr)
    if run.returncode:raise RuntimeError(run.stderr.decode('utf-8',errors='replace'))
    actual=json.loads((args.out/'native.json').read_text('utf-8'));checks=[]
    def equivalent(a,b):
        if isinstance(a,float) and isinstance(b,(int,float)):return math.isclose(a,b,rel_tol=2e-13,abs_tol=1e-13)
        if isinstance(a,dict) and isinstance(b,dict):return a.keys()==b.keys() and all(equivalent(a[k],b[k]) for k in a)
        return a==b
    for n,(job,want,got) in enumerate(zip(jobs,expected,actual)):
        if job['action']!='profile':passed=('error' in got) if 'error' in want else equivalent(want,got)
        else:
            keys=['source','scope','frame_sha256','range','requested_samples','warmup_replays','include_writes','single_replay_sample','statistics_shader_instrumentation','query_completion','so_count_reconstruction','test_storage','test_draw_auto','experiment','quantile_method','notes']
            passed='error' not in got and all(want[k]==got[k] for k in keys)
            if passed:
                passed=want['execution_device']==got['execution_device']
                passed &= [[r[k] for k in ['event','api','enabled','boundary']] for r in want['events']]==[[r[k] for k in ['event','api','enabled','boundary']] for r in got['events']]
                for row in got['events']:
                    values=[]
                    for p in got['passes']:
                        v=p['events'][str(row['event'])]
                        computed=gpu_profile.timing(p['frequency_hz'],p['disjoint'],v['start_tick'],v['end_tick'],p['envelope']['start_tick'],p['envelope']['end_tick'])
                        passed &= equivalent(v,computed)
                        if v['available']:values.append(v['elapsed_ms'])
                    dist=gpu_profile.distribution(values)
                    passed &= equivalent(dist,{key:row[key] for key in dist}) and row['invalid_samples']==job['request']['samples']-len(values)
                ranked=sorted([r for r in got['events'] if r['median_ms'] is not None],key=lambda r:(-r['median_ms'],r['event']))
                passed &= all(row['rank']==rank for rank,row in enumerate(ranked,1))
                passed &= [p['replay_generation'] for p in got['passes']]==[p['replay_generation'] for p in want['passes']]
                passed &= [p['replay_counts'] for p in got['passes']]==[p['replay_counts'] for p in want['passes']]
                passed &= got['replay_generation']==want['replay_generation']
                envelopes=[]
                for p in got['passes']:
                    v=p['envelope'];computed=gpu_profile.timing(p['frequency_hz'],p['disjoint'],v['start_tick'],v['end_tick'],v['start_tick'],v['end_tick'])
                    passed &= equivalent(v,computed)
                    if v['available']:envelopes.append(v['elapsed_ms'])
                passed &= equivalent(gpu_profile.distribution(envelopes),got['envelope'])
                folder=Path(job['out'])
                progress=json.loads((folder/'profile-progress.json').read_text('utf-8'))
                passed &= progress==dict(phase='complete',completed=job['request']['samples'],samples=job['request']['samples'],warmup=job['request']['warmup'],events=len(got['events']))
                with (folder/'profile.csv').open(encoding='utf-8-sig',newline='') as stream: csv_rows=list(csv.DictReader(stream))
                passed &= len(csv_rows)==len(got['events'])
                for row,record in zip(got['events'],csv_rows):
                    for key,text in record.items():
                        value=row[key]
                        passed &= (text=='' if value is None else text==str(value) if isinstance(value,(str,bool,int)) else math.isclose(float(text),value,rel_tol=2e-13,abs_tol=1e-13))
                with (folder/'profile-samples.csv').open(encoding='utf-8-sig',newline='') as stream: csv_rows=list(csv.DictReader(stream))
                passed &= len(csv_rows)==len(got['events'])*len(got['passes'])
                for record in csv_rows:
                    p=got['passes'][int(record['sample'])];v=p['events'][record['event']]
                    passed &= int(record['frequency_hz'])==p['frequency_hz'] and record['disjoint']==str(p['disjoint'])
                    passed &= int(record['start_tick'])==v['start_tick'] and int(record['end_tick'])==v['end_tick']
                    for key in ['elapsed_ms','offset_ms']:
                        passed &= record[key]=='' if v[key] is None else math.isclose(float(record[key]),v[key],rel_tol=2e-13,abs_tol=1e-13)
                    passed &= record['unavailable_reason']==(v['unavailable_reason'] or '')
        checks.append(dict(index=n,action=job['action'],passed=bool(passed)))
        if not passed:(args.out/f'mismatch-{n}.json').write_text(json.dumps(dict(job=job,expected=want,actual=got),indent=2),'utf-8')
    report=dict(passed=len(actual)==len(expected) and all(c['passed'] for c in checks),cases=len(checks),gpu_cases=len(gpu),checks=checks)
    (args.out/'validation.json').write_text(json.dumps(report,indent=2),'utf-8')
    print(json.dumps({k:v for k,v in report.items() if k!='checks'}));return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
