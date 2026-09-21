"""Compare original private Quad depth preparation with the native GPU helper."""
import argparse
import ctypes as c
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    for name in ('reference', 'probe', 'qt-bin', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    a = parser.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'reference').mkdir()
    sys.path[:0] = [str(a.reference / 'standalone'), str(a.reference / 'tools')]
    from dx11 import Device, P, U, I, method
    from frame import Frame
    from engine import Engine
    from events import draw_event, execution_parameters
    from state import decode_state
    from quad_targets import select
    from quad_depth import prepare
    from quad_counter import preserve_high_uavs, fingerprint_outputs
    from coverage_viewport import extent
    from quad_fixtures import fixture as base, CASES, indirect_fixture
    from validate_quad_targets import fixture as targets
    from validate_coverage_dimensions import fixture as dimensions
    from validate_coverage_buffer_rtv import fixture as buffer
    from validate_coverage_viewport import fixture as viewport
    from validate_pre_raster_uav import fixture as writer
    from validate_so_passthrough import fixture as passthrough, multi_fixture
    from validate_class_linkage import fixture as classes
    from validate_predication import fixture as predicate
    from validate_buffer_edits import experiment
    from experiments import blob
    from shaders import compile_hlsl
    from stream_output import unbind_targets

    fixtures = []
    def add(label, create, event=100, edits=None, error=False, **options):
        path = out / (label + '.gpa_frame')
        create(path)
        fixtures.append((label, dict(capture=str(path), event=event, **options), edits, error))
    for name in CASES:
        add('depth-' + name, lambda path: base(path, name), error=name == 'null_ps')
    for samples in (1, 2, 4, 8):
        for array in (False, True):
            for no_depth in (False, True):
                add(f'target-{samples}-{array}-{no_depth}',
                    lambda path: targets(path, samples=samples, array=array, no_depth=no_depth))
            add(f'depth-only-{samples}-{array}',
                lambda path: targets(path, samples=samples, array=array, depth_only=True))
    for dim in ('1d', '3d'):
        for array in (False, True):
            add(f'dimension-{dim}-{array}', lambda path: dimensions(path, dim=dim, array=array), 500)
    for readonly in (False, True):
        for array in (False, True):
            add(f'depth1d-{readonly}-{array}',
                lambda path: dimensions(path, depth=True, readonly=readonly, array=array), 500)
    for fmt in (28, 42):
        add(f'buffer-{fmt}', lambda path: buffer(path, fmt=fmt), 500)
    for kind in ('typed', 'counter', 'null'):
        add('viewport-' + kind, lambda path: viewport(path, kind=kind, negative=True), error=kind == 'null')
    for stage in ('vs', 'hs', 'ds', 'gs'):
        for slot in (1, 8, 63):
            add(f'writer-{stage}-{slot}', lambda path: writer(path, stage, slot, True))
    for stage in ('vs', 'hs', 'ds', 'gs', 'ps'):
        for created in (False, True):
            add(f'classes-{stage}-{created}', lambda path: classes(path, stage=stage, created=created))
    for stream in (0, 1, 2, 3, 0xffffffff):
        add('stream-' + str(stream), lambda path: multi_fixture(path, stream))
        add('stream-suspended-' + str(stream), lambda path: multi_fixture(path, stream),
            copy_so=False, suspend_so=True)
    for signature in (False, True):
        for tessellated in (False, True):
            add(f'passthrough-{signature}-{tessellated}',
                lambda path: passthrough(path, profile='ds_5_0' if tessellated else 'vs_5_0',
                                         signature_only=signature, tessellated=tessellated))
    for indexed in (False, True):
        add('indirect-' + str(indexed), lambda path: indirect_fixture(path, indexed))
    for visible in (False, True):
        for value in (0, 1):
            add(f'predicate-{visible}-{value}', lambda path: predicate(path, visible, value), 200)
    add('mismatch', lambda path: base(path, 'less'), mismatch=True, error=True)
    add('submit-failure', lambda path: writer(path, 'vs', 63, True), **{'throw': True}, error=True)
    add('submit-failure-so', lambda path: multi_fixture(path, 2), **{'throw': True}, error=True)
    add('disabled', lambda path: base(path, 'less'), edits=[dict(kind='enabled', event=100, value=False)])
    shader = out / 'depth-edit.dxbc'
    shader.write_bytes(compile_hlsl('float main():SV_Depth{return .3125;}', 'ps_5_0')[0])
    add('edited-ps-depth', lambda path: base(path, 'less'),
        edits=[dict(kind='shader', resource=12, asset=blob(shader))])

    def bindings(device):
        values = []
        rt, ds = (P*8)(), P()
        device.call(89, [U, P, P], 8, rt, c.byref(ds))
        values.extend(rt); values.append(ds.value)
        count = 64 if device.feature_level >= 0xb100 else 8
        uavs = (P*count)()
        device.call(90, [U, P, P, U, U, P], 0, None, None, 0, count, uavs)
        values.extend(uavs)
        ps, instances, count = P(), (P*256)(), U(256)
        device.call(74, [P, P, P], c.byref(ps), instances, c.byref(count))
        values.append(ps.value); values.extend(instances[:count.value])
        depth, reference, so = P(), U(), (P*4)()
        device.call(92, [P, P], c.byref(depth), c.byref(reference))
        values.append(depth.value)
        device.call(93, [U, P], 4, so)
        values.extend(so)
        for obj in values:
            if obj: method(obj, 2, U, [])(obj)
        return values + [reference.value]

    jobs, expected, labels = [], [], []
    for label, job, edits, rejected in fixtures:
        for driver in ('hardware', 'warp'):
            job = dict(job, warp=driver == 'warp')
            with Frame(Path(job['capture'])) as frame:
                project = None
                if edits:
                    job['experiment'] = str(out / (label + '-' + driver + '-experiment.json'))
                    project = experiment(frame, Path(job['experiment']), edits)
                device = Device(driver)
                try:
                    engine = Engine(frame, device, experiment=project)
                    engine.replay(job['event'], before=True, readback=False)
                    event = draw_event(frame, frame.entries[job['event']])
                    state = decode_state(frame.payload(event['state_id']))
                    with project.inputs(engine, event) if project else nullcontext():
                        engine.bind(state)
                        if project: project.pipeline(engine, event, state)
                        result = dict(before=fingerprint_outputs(engine, state))
                        original, counts = bindings(device), dict(engine.counts)
                        try:
                            selected, metadata = select(engine, state)
                            width, height = (selected['width'], selected['height']) if selected else extent(device)[:2]
                            if job.get('mismatch'): width += 1
                            def submit():
                                if job.get('throw'): raise RuntimeError('Injected Quad submit failure')
                                if project and not project.events.get(event['id'], {}).get('enabled', True): return 0
                                if job.get('suspend_so'): unbind_targets(device)
                                parameters = execution_parameters(frame, event, engine)
                                args = list(parameters.values())
                                types = [I if key == 'base_vertex' else U for key in parameters]
                                if 'argument_buffer' in parameters:
                                    types[0] = P; args[0] = engine.resources.get(args[0])
                                device.call(event['slot'], types, *args)
                                return 1
                            view, report, digest = prepare(engine, event, state, width, height, submit,
                                preserve_high_uavs, copy_so=job.get('copy_so', True), selected=selected, target_metadata=metadata)
                            result.update(metadata=report, digest=digest(), digest_again=digest())
                        except (ValueError, RuntimeError) as error:
                            result['error'] = str(error)
                        result.update(bindings_restored=bindings(device) == original,
                                      counts_unchanged=dict(engine.counts) == counts,
                                      after=fingerprint_outputs(engine, state))
                        (out / 'reference' / (driver + '-' + label + '.json')).write_text(json.dumps(result, indent=2))
                        assert ('error' in result) == rejected, (label, driver, result.get('error'))
                        assert result['bindings_restored'] and result['counts_unchanged'], (label, result)
                        assert result['before'] == result['after'], label
                finally:
                    device.close()
            jobs.append(job); expected.append(result); labels.append(driver + '-' + label)
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
    failures = [dict(label=label, expected=want, actual=got)
                for label, want, got in zip(labels, expected, actual) if want != got]
    report = dict(passed=not failures, cases=len(jobs), rejected=sum('error' in row for row in expected),
                  failures=failures, probe_sha256=hashlib.sha256(a.probe.read_bytes()).hexdigest(),
                  reference_sources={name: hashlib.sha256((a.reference / 'standalone' / name).read_bytes()).hexdigest()
                                     for name in ('quad_depth.py', 'quad_counter.py', 'quad_targets.py')})
    (out / 'validation.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v for k, v in report.items() if k != 'failures'}))
    assert not failures, [row['label'] for row in failures[:10]]


if __name__ == '__main__':
    main()
