"""Compare original and native final-geometry Quad helpers on actual GPU frames."""
import argparse
import ctypes as c
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--qt-bin', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sys.path[:0] = [str(a.reference / 'standalone'), str(a.reference / 'tools')]
    from frame import Frame
    from engine import Engine
    from dx11 import Device, P, U
    from events import draw_event
    from state import decode_state
    from shaders import compile_hlsl
    from quad_post_transform import PostTransformCounter
    from uav_counters import read_counter
    from msaa import inspect_msaa
    from experiments import blob
    from validate_buffer_edits import experiment
    from validate_quad_post_transform import fixture
    from validate_quad_linear import fixture as linear
    from validate_quad_targets import fixture as targets
    from validate_pre_raster_uav import fixture as writer
    from validate_so_passthrough import fixture as passthrough, multi_fixture, DOMAIN
    from validate_tess_linear import fixture as tess
    from validate_class_linkage import fixture as classes

    fixtures = []
    def add(label, create, textures=(30, 60), buffers=(), counters=(), edits=None, event=100):
        path = out / (label + '.gpa_frame')
        create(path)
        fixtures.append((label, dict(capture=str(path), event=event, textures=list(textures),
                                    buffers=list(buffers), counters=list(counters)), edits))
    for kind in ('tess1', 'tess2', 'gs', 'strip', 'clip', 'cull', 'viewport'):
        add(kind, lambda path: fixture(path, kind))
    for top in (1, 2):
        for generated in (False, True):
            add(f'linear-{top}-{generated}', lambda path: linear(path, topology=top, generated=generated))
    for domain, point in (('isoline', False), ('isoline', True), ('tri', True), ('quad', True)):
        for geometry in (False, True):
            add(f'tess-{domain}-{point}-{geometry}', lambda path: tess(path, domain, point, geometry=geometry))
    for stage in ('vs', 'hs', 'ds', 'gs'):
        for slot in (1, 8, 63):
            for counter in (False, True):
                add(f'uav-{stage}-{slot}-{counter}', lambda path: writer(path, stage, slot, counter),
                    buffers=(300,), counters=(302,) if counter else ())
        for created in (False, True):
            add(f'class-{stage}-{created}', lambda path: classes(path, stage=stage, created=created),
                textures=(30,), buffers=(70,))
    for signature_only in (False, True):
        for tessellated in (False, True):
            add(f'passthrough-{signature_only}-{tessellated}',
                lambda path: passthrough(path, profile='ds_5_0' if tessellated else 'vs_5_0',
                                         signature_only=signature_only, tessellated=tessellated),
                textures=(50,), buffers=(40,), event=150)
    for stream in (0, 1, 2, 3, 0xffffffff):
        add(f'stream-{stream}', lambda path: multi_fixture(path, stream),
            textures=(50,), buffers=(40, 42, 44, 46), event=150)
    for clip in (False, True):
        add(f'array-{clip}', lambda path: targets(path, array=True, clip=clip))
    for samples in (2, 4, 8):
        for array in (False, True):
            label = f'msaa-{samples}-{array}'
            add(label, lambda path: targets(path, samples=samples, array=array), textures=())
            fixtures[-1][1]['msaa'] = [dict(resource=30, samples=samples, format=28),
                                       dict(resource=60, samples=samples, format=45)]
    add('disabled', lambda path: fixture(path, 'gs'), edits=[dict(kind='enabled', event=100, value=False)])
    replacement = out / 'offscreen-ds.dxbc'
    replacement.write_bytes(compile_hlsl(DOMAIN.replace('v.color=', 'v.p.xy+=100;v.color='), 'ds_5_0')[0])
    add('edited-ds', lambda path: fixture(path, 'tess1'),
        edits=[dict(kind='shader', resource=82, asset=blob(replacement))])

    def snapshot(engine, job):
        result = dict(textures={str(id): hashlib.sha256(engine.read_texture(id)).hexdigest() for id in job['textures']},
                      buffers={str(id): hashlib.sha256(engine.read_buffer(id)).hexdigest() for id in job['buffers']},
                      counters={str(id): read_counter(engine.device, engine.resources.get(id)) for id in job['counters']})
        if 'msaa' in job:
            result['msaa'] = {str(item['resource']): [hashlib.sha256(inspect_msaa(
                engine.device, engine.resources.get(item['resource']), engine.frame.resource(item['resource']),
                sample, item['format'])[1]).hexdigest() for sample in range(item['samples'])] for item in job['msaa']}
        return result

    jobs, expected, labels = [], [], []
    ps_code = compile_hlsl('float4 main(float4 p:SV_Position,uint id:SV_PrimitiveID):SV_Target{return float4(id+1,0,1,1);}', 'ps_5_0')[0]
    for label, base, edits in fixtures:
        for driver in ('hardware', 'warp'):
            job = dict(base, warp=driver == 'warp')
            with Frame(Path(job['capture'])) as frame:
                project = None
                if edits:
                    job['experiment'] = str(out / (label + '-' + driver + '-experiment.json'))
                    project = experiment(frame, Path(job['experiment']), edits)
                device = Device(driver)
                try:
                    engine = Engine(frame, device, experiment=project)
                    event = draw_event(frame, frame.entries[job['event']])
                    state = decode_state(frame.payload(event['state_id']))
                    engine.replay(until=job['event'], before=True, readback=False)
                    with project.inputs(engine, event) if project else nullcontext():
                        engine.bind(state)
                        if project:
                            project.pipeline(engine, event, state)
                        value = dict(before=snapshot(engine, job))
                        try:
                            helper = PostTransformCounter(engine, event, state)
                            value.update(after_capture=snapshot(engine, job), metadata=helper.metadata(),
                                         signatures=helper.output_signatures)
                            device.call(9, [P, P, U], device.shader('ps', ps_code), None, 0)
                            depth = device.descriptor(21, struct.pack('<13I', 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), 'Quad test depth')
                            device.call(36, [P, U], depth, 0)
                            device.call(35, [P, P, U], None, None, 0xffffffff)
                            value['submissions'], value['passes'] = [], []
                            for _ in range(2):
                                value['submissions'].append(helper.submit())
                                value['passes'].append(snapshot(engine, job))
                        except (ValueError, RuntimeError) as error:
                            value.update(error=str(error), after_failure=snapshot(engine, job))
                    value['counts'] = dict(engine.counts)
                    # These synthetic prefixes have one OM setter in each UAV
                    # fixture and no other auxiliary command. Verify its Python
                    # decoded-record count independently before normalization.
                    assert value['counts'].get('state_or_auxiliary_records', 0) == sum(
                        e.category == 7 and e.type == 0x3500 and e.id < job['event'] for e in frame.entries.values())
                finally:
                    device.close()
            jobs.append(job)
            expected.append(value)
            labels.append(driver + '-' + label)
            rejection = label == 'stream-4294967295' or (driver == 'warp' and label == 'tess-isoline-False-False')
            assert ('error' in value) == rejection, (labels[-1], value.get('error'))
            (out / 'progress.json').write_text(json.dumps(dict(cases=len(jobs), last=labels[-1])))
    (out / 'jobs.json').write_text(json.dumps(jobs))
    (out / 'expected.json').write_text(json.dumps(expected, indent=2))
    env = {k: v for k, v in os.environ.items() if k.upper() in ('SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'APPDATA', 'LOCALAPPDATA')}
    env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + str(Path(os.environ['SystemRoot']) / 'System32')
    run = subprocess.run([str(a.probe.resolve()), '--probe', str(out / 'jobs.json'), str(out / 'actual.json')],
                         env=env, capture_output=True, timeout=300)
    (out / 'native.log').write_bytes(run.stdout + run.stderr)
    assert run.returncode == 0, run.stderr
    actual = json.loads((out / 'actual.json').read_text())
    assert len(actual) == len(expected)
    def counts(values):
        result = {}
        for key, value in values.items():
            if key.endswith('_records') and key.startswith(('Draw', 'Dispatch')):
                assert value == values.get(key[:-8]), (key, values)
            elif key != 'state_or_auxiliary_records' and not key.startswith(('meta_', 'inspection_')):
                result[key] = value
        return result
    failures = []
    for label, want, got in zip(labels, expected, actual):
        want, got = dict(want), dict(got)
        for value in (want, got):
            if 'counts' in value:
                value['counts'] = counts(value['counts'])
        equal = want == got
        isolated = all(value.get('before') == value.get('after_capture', value.get('after_failure')) for value in (want, got))
        for value in (want, got):
            for image in value.get('passes', []):
                isolated &= all(image[key] == value['before'][key] for key in ('buffers', 'counters'))
        if not equal or not isolated:
            failures.append(dict(label=label, isolated=isolated, expected=want, actual=got))
    report = dict(passed=not failures, cases=len(jobs), expected_rejections=sum('error' in x for x in expected),
                  failures=failures, probe_sha256=hashlib.sha256(a.probe.read_bytes()).hexdigest(),
                  reference_sources={name: hashlib.sha256((a.reference / 'standalone' / name).read_bytes()).hexdigest()
                                     for name in ('quad_post_transform.py', 'post_transform.py', 'coverage_isolated.py')})
    (out / 'validation.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v for k, v in report.items() if k != 'failures'}))
    assert not failures, [f['label'] for f in failures[:10]]


if __name__ == '__main__':
    main()
