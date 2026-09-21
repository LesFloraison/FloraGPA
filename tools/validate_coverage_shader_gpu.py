"""Run original GPU fixtures with only DXBC marker/relocation supplied by C++.

This is development evidence for shader transformations, not a claim that the
Python diagnostic engine or its UI has already been ported.
"""
import argparse
import base64
import hashlib
import importlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--reference',type=Path,required=True)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--qt-bin',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--suite',choices=['fragment','isolated','viewport','arrays','pre-raster'],required=True)
    args=parser.parse_args();args.out=args.out.resolve();args.out.mkdir(parents=True,exist_ok=False)
    sys.path[:0]=[str(args.reference/'standalone'),str(args.reference/'tools')]
    import dxbc_patch,dxbc_uav
    env=dict(os.environ);env['PATH']=str(args.qt_bin.resolve())+os.pathsep+str(Path(os.environ['SystemRoot'])/'System32')
    cache={};calls=[]
    def native(raw,action,**kwargs):
        job=dict(action=action,input=base64.b64encode(raw).decode('ascii'),**kwargs)
        key=json.dumps(job,sort_keys=True)
        if key in cache:return cache[key]
        index=len(cache);input_path=args.out/f'native-{index}.json';output_path=args.out/f'native-{index}-result.json'
        input_path.write_text(json.dumps([job]),'utf-8')
        run=subprocess.run([str(args.probe.resolve()),'--probe',str(input_path),str(output_path)],env=env,capture_output=True,timeout=60)
        if run.returncode:raise RuntimeError(run.stderr.decode('utf-8',errors='replace'))
        result=json.loads(output_path.read_text())[0]
        if 'error' in result:raise ValueError(result['error'])
        result['output']=base64.b64decode(result['output']);cache[key]=result
        calls.append(dict(action=action,input_sha256=hashlib.sha256(raw).hexdigest(),output_sha256=hashlib.sha256(result['output']).hexdigest()))
        return result
    def add(raw,slot):return native(raw,'add',slot=slot)['output']
    def replace(raw,slot,keep_alpha=False,array_index=None,array_routed=False,array_source=None):
        return native(raw,'replace',slot=slot,keep_alpha=keep_alpha,array_index=array_index,array_routed=array_routed,array_source=array_source)['output']
    def relocate(raw,mapping,slot_count=8):
        r=native(raw,'relocate',mapping=list(mapping.items()),slot_count=slot_count);return r['output'],r['declared']
    def reserve(raw,bound_slots,slot_count=8):
        r=native(raw,'reserve',bound=sorted(bound_slots),slot_count=slot_count);return r['output'],{int(k):v for k,v in r['mapping'].items()}
    dxbc_patch.coverage_shader=add;dxbc_patch.replacement_coverage_shader=replace
    dxbc_uav.relocate=relocate;dxbc_uav.reserve_rt0=reserve
    target=args.out/'gpu'
    if args.suite!='arrays':
        names=dict(fragment='validate_fragment_coverage',isolated='validate_coverage_isolated',viewport='validate_coverage_viewport',**{'pre-raster':'validate_pre_raster_coverage'})
        module=importlib.import_module(names[args.suite]);sys.argv=[module.__file__,'--out',str(target)];module.main()
        checks=json.loads((target/'validation.json').read_text('utf-8'))['checks']
    else:
        # Exercise the fixture module directly so its unrelated Python/Tk UI and
        # subprocess checks cannot silently bypass the injected native shader code.
        m=importlib.import_module('validate_coverage_viewport_arrays');target.mkdir();checks=[]
        cases=[('overlap',{}),('reverse-order',dict(indices=(2048,1))),('uint-max',dict(indices=(0xffffffff,65535))),
               ('duplicate-index',dict(indices=(7,7))),('unused-PS-index',dict(input_index=False)),('stripped',dict(stripped=True)),
               ('sparse-uav',dict(slots=(0,7))),('eight-uavs',dict(slots=tuple(range(8)))),('counter',dict(counter=True)),
               ('color-return',dict(color=True,early=True,slots=(1,))),('early-return',dict(early=True)),('negative',dict(negative=True)),
               ('predicate-false',dict(predicate=True)),('implicit-zero',dict(routed=False,indices=(0,))),
               ('vs-route',dict(producer='vs',input_index=False)),('ds-route',dict(producer='ds',indices=(2048,),input_index=False)),
               ('dynamic-PS',dict(dynamic=True,input_index=False)),('gs-stream-one',dict(stream=1,input_index=False))]
        for driver in ['hardware','warp']:
            for name,options in cases:
                path=m.fixture(target/f'{driver}-{name}.gpa_frame',**options);base=m.run(path,target/'unused',driver,False)
                indices=options.get('indices',(1,2048))
                for index in [None,*sorted(set(indices)),9]:
                    actual=m.run(path,target/f'{driver}-{name}-{index}',driver,index=index)
                    w,h,pixels=actual['mask'];mask={(n%w,n//w) for n,v in enumerate(pixels[::4]) if v}
                    assert mask==m.coordinates(base,indices,index),(driver,name,index,'mask')
                    assert all(base[k]==actual[k] for k in ['buffers','counters','counts','occlusion']),(driver,name,index,'side effects')
                    r=actual['report'];assert r['array_index_selection']==index and r['render_target_array_routing']==options.get('routed',True)
                    checks.append(dict(name=f'{driver}-{name}-{index}',passed=True))
            for n,options in enumerate([dict(early=True),dict(counter=True),dict(color=True,early=True),dict(input_index=False)]):
                path=m.fixture(target/f'{driver}-effects-{n}.gpa_frame',slots=(1,),**options);base=m.run(path,target/'unused',driver,False)
                for index in [1,2048,9]:
                    actual=m.run(path,target/'unused',driver,False,index=index,direct=True)
                    assert base['buffers']==actual['buffers'] and base['counters']==actual['counters']
                    checks.append(dict(name=f'{driver}-nonselected-effects-{n}-{index}',passed=True))
    assert calls and checks and all(c['passed'] for c in checks)
    report=dict(passed=True,suite=args.suite,checks=checks,native_transformations=calls)
    (args.out/'validation.json').write_text(json.dumps(report,indent=2),'utf-8')
    print(json.dumps(dict(passed=True,suite=args.suite,checks=len(checks),native_transformations=len(calls))))


if __name__=='__main__':main()
