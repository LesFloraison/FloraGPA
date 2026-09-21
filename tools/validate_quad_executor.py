"""Full native Quad reports/artifacts and original-event replay against Python."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    p = argparse.ArgumentParser()
    for name in ('reference','probe','qt-bin','out'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--phase',choices=('all','reference','native'),default='all')
    p.add_argument('--suite',choices=('smoke','full','real'),default='full')
    a = p.parse_args(); out = a.out.resolve()
    sys.path[:0] = [str(a.reference/'standalone'),str(a.reference/'tools')]
    names = ['quad.py','quad_counter.py','quad_counter.hlsl','quad_targets.py','quad_depth.py',
             'quad_serial.py','quad_post_transform.py','pre_raster_uav.py','coverage_isolated.py']
    sources = {name:hashlib.sha256((a.reference/'standalone'/name).read_bytes()).hexdigest() for name in names}
    if a.phase != 'native':
        out.mkdir(parents=True,exist_ok=False)
        from frame import Frame
        from engine import Engine
        from dx11 import Device
        from state import decode_state
        from events import draw_event
        from quad import export_quad
        from quad_counter import fingerprint_outputs
        from image import read_png
        from quad_fixtures import fixture as simple, CASES, indirect_fixture
        from validate_quad_post_transform import fixture as final
        from validate_quad_targets import fixture as targets
        from validate_quad_linear import fixture as linear
        from quad_strip_fixtures import fixture as strips
        from validate_coverage_dimensions import fixture as dimensions
        from validate_coverage_buffer_rtv import fixture as buffer
        from validate_coverage_viewport import fixture as viewport
        from validate_pre_raster_uav import fixture as writer
        from validate_so_passthrough import fixture as passthrough, multi_fixture
        from validate_class_linkage import fixture as classes
        from validate_buffer_edits import experiment
        from experiments import blob
        from shaders import compile_hlsl
        cases = []
        def add(label,create,event=100,edits=None,rejection=None,**options):
            path = out/(label+'.gpa_frame'); create(path)
            cases.append((label,dict(capture=str(path),event=event,**options),edits,rejection))
        basic = () if a.suite=='real' else tuple(CASES) if a.suite=='full' else ('less','discard_half','ps_depth_far','stencil_fail','null_ps')
        for name in basic:
            add('depth-'+name,lambda path:simple(path,name),rejection='prepared' if name=='null_ps' else None)
        for kind in (() if a.suite=='real' else ('tess1','tess2','gs','strip','clip','cull','viewport') if a.suite=='full' else ('gs','tess1')):
            add('final-'+kind,lambda path:final(path,kind))
        for stage in (() if a.suite=='real' else ('vs','hs','ds','gs') if a.suite=='full' else ('vs',)):
            for slot in ((0,1,4,8,63) if a.suite=='full' else (1,)):
                add(f'writer-{stage}-{slot}',lambda path:writer(path,stage,slot,True))
        if a.suite=='full':
            for samples in (1,2,4,8):
                for array in (False,True):
                    for no_depth in (False,True):
                        add(f'target-{samples}-{array}-{no_depth}',lambda path:targets(path,samples=samples,array=array,no_depth=no_depth))
            for array in (False,True):
                add('depth-only-'+str(array),lambda path:targets(path,array=array,depth_only=True))
            for layer in (1,2):
                add('selected-layer-'+str(layer),lambda path:targets(path,array=True),layer=layer)
            for dim in ('1d','3d'):
                for array in (False,True):
                    add(f'dimension-{dim}-{array}',lambda path:dimensions(path,dim=dim,array=array),500)
            for readonly in (False,True):
                add('depth1d-'+str(readonly),lambda path:dimensions(path,depth=True,readonly=readonly),500)
            for fmt in (28,42,41):
                add('buffer-'+str(fmt),lambda path:buffer(path,fmt=fmt),500)
            for kind in ('typed','counter'):
                # This original fixture leaves its hidden counter uninitialized.
                # Seed it through the supported experiment API so each replay
                # starts from the same value (and takes the actual write branch).
                add('viewport-'+kind,lambda path:viewport(path,kind=kind,negative=False),
                    edits=[dict(kind='initial_uav_counter',view=1002,value=5)] if kind=='counter' else None)
            for topology in (1,2):
                for generated in (False,True):
                    add(f'linear-{topology}-{generated}',lambda path:linear(path,topology=topology,generated=generated))
            for indexed in (False,True):
                add('indirect-'+str(indexed),lambda path:indirect_fixture(path,indexed))
                for pattern in ('ordinary','degenerate','restart','zero') if indexed else ('ordinary','zero'):
                    add(f'strip-{indexed}-{pattern}',lambda path:strips(path,fmt=42 if indexed else None,pattern=pattern))
            for stream in (0,1,2,3,0xffffffff):
                add('so-'+str(stream),lambda path:multi_fixture(path,stream),rejection='all' if stream==0xffffffff else None)
            for signature in (False,True):
                add('passthrough-'+str(signature),lambda path:passthrough(path,signature_only=signature))
            for stage in ('vs','hs','ds','gs','ps'):
                add('classes-'+stage,lambda path:classes(path,stage=stage,created=True))
            add('disabled-final',lambda path:final(path,'gs'),edits=[dict(kind='enabled',event=100,value=False)])
            edit = out/'edited-vs.dxbc'
            edit.write_bytes(compile_hlsl('float4 main(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,.3125,1);}','vs_5_0')[0])
            add('edited-vs',lambda path:simple(path,'less'),edits=[dict(kind='shader',resource=10,asset=blob(edit))])
            add('invalid-target',lambda path:simple(path,'less'),target='rt7',rejection='all')
        if a.suite=='real':
            for label,name,event in (
                ('gf2-first','GF2_Exilium_2026_03_03__00_19_35.gpa_frame',113),
                ('bf1-mrt','bf1_2026_01_21__16_53_05.gpa_frame',3313)):
                cases.append((label,dict(capture=str((a.reference/name).resolve()),event=event),None,None))
        jobs,expected,labels = [],[],[]
        for label,base_job,edits,rejection in cases:
            for driver in ('hardware','warp'):
                for mode in ('prepared','before','none'):
                    key = driver+'-'+label+'-'+mode
                    job = dict(base_job,warp=driver=='warp',depth=mode)
                    directory = out/'reference'/key
                    with Frame(Path(job['capture'])) as frame:
                        project = None
                        if edits:
                            job['experiment']=str(out/(key+'-experiment.json'))
                            project=experiment(frame,Path(job['experiment']),edits)
                        device=Device(driver)
                        try:
                            engine=Engine(frame,device,experiment=project)
                            event=draw_event(frame,frame.entries[job['event']])
                            state=decode_state(frame.payload(event['state_id']))
                            engine.replay(job['event'],readback=False)
                            baseline=fingerprint_outputs(engine,state); counts=dict(engine.counts)
                            try:
                                report=export_quad(engine,job['event'],directory,mode,target=job.get('target','auto'),layer=job.get('layer'))
                                actual=fingerprint_outputs(engine,state)
                                data=directory/'data'
                                value=dict(report=report,counter_report=json.loads((data/'result.json').read_text()),
                                           preview_sha256=hashlib.sha256(read_png(data/'quad_counts.png')[2]).hexdigest(),
                                           storage_sha256=[hashlib.sha256((data/(name+'.u32le')).read_bytes()).hexdigest() for name in ('locks','counts','live','histogram','reference')],
                                           after=actual,baseline_unchanged=actual==baseline and counts==dict(engine.counts))
                                assert value['baseline_unchanged'], key
                                assert report['original_storage_unchanged'] and report['depth_test']==(mode!='none'),key
                            except (ValueError,RuntimeError) as error:
                                value=dict(error=str(error))
                        finally: device.close()
                    (out/(key+'-reference.json')).write_text(json.dumps(value,indent=2))
                    assert ('error' in value)==(rejection in ('all',mode)),(key,value.get('error'))
                    jobs.append(job);expected.append(value);labels.append(key)
                    (out/'progress.json').write_text(json.dumps(dict(cases=len(jobs),last=key)))
        (out/'jobs.json').write_text(json.dumps(jobs))
        (out/'expected.json').write_text(json.dumps(expected,indent=2))
        (out/'labels.json').write_text(json.dumps(labels))
        (out/'reference-sources.json').write_text(json.dumps(sources,indent=2))
        if a.phase=='reference':
            print(json.dumps(dict(reference_complete=True,cases=len(jobs))));return
    else:
        assert json.loads((out/'reference-sources.json').read_text())==sources,'Reference changed'
        jobs=json.loads((out/'jobs.json').read_text());expected=json.loads((out/'expected.json').read_text());labels=json.loads((out/'labels.json').read_text())
    run_dir=out/('native-'+str(len(list(out.glob('native-*')))+1));run_dir.mkdir()
    for job,label in zip(jobs,labels): job['out']=str(run_dir/label)
    (run_dir/'jobs.json').write_text(json.dumps(jobs))
    env={k:v for k,v in os.environ.items() if k.upper() in ('SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA')}
    env['PATH']=str(a.qt_bin.resolve())+os.pathsep+str(Path(os.environ['SystemRoot'])/'System32')
    run=subprocess.run([str(a.probe.resolve()),'--probe',str(run_dir/'jobs.json'),str(run_dir/'actual.json')],env=env,capture_output=True,timeout=1200)
    (run_dir/'native.log').write_bytes(run.stdout+run.stderr)
    assert run.returncode==0,run.stderr
    actual=json.loads((run_dir/'actual.json').read_text());assert len(actual)==len(expected)
    # Native Replay adds event context to observer failures. Preserve raw evidence;
    # accept only this exact, verified Draw envelope, never arbitrary error text.
    from frame import Frame
    from events import draw_event
    failures=[]
    contextual_errors=0
    for label,job,want,got in zip(labels,jobs,expected,actual):
        if want==got:
            continue
        if set(want)==set(got)=={'error'}:
            with Frame(Path(job['capture'])) as frame:
                event=draw_event(frame,frame.entries[job['event']])
            prefix=f"Event {job['event']} ({event['name']}): "
            if got['error']==prefix+want['error']:
                contextual_errors+=1
                continue
        failures.append(dict(label=label,expected=want,actual=got))
    report=dict(passed=not failures,cases=len(jobs),rejected=sum('error' in row for row in expected),failures=failures,
                contextual_errors=contextual_errors,
                probe_sha256=hashlib.sha256(a.probe.read_bytes()).hexdigest(),reference_sources=sources)
    (run_dir/'validation.json').write_text(json.dumps(report,indent=2))
    print(json.dumps({k:v for k,v in report.items() if k not in ('failures','reference_sources')}))
    assert not failures,[row['label'] for row in failures[:12]]


if __name__=='__main__':main()
