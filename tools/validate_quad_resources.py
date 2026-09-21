"""Compare native Quad target allocation and complete output fingerprints."""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    for name in ('reference', 'probe', 'qt-bin', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    a = parser.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sys.path[:0] = [str(a.reference / 'standalone'), str(a.reference / 'tools')]
    from frame import Frame
    from engine import Engine
    from dx11 import Device, P, U, I, F, Mapped, method, checked
    from state import decode_state
    from events import draw_event
    from quad_targets import select, dummy, producer, storage_bytes
    from quad_counter import fingerprint_outputs
    from coverage_viewport import extent
    from coverage_isolated import view_resource
    from validate_quad_targets import fixture as target_fixture
    from validate_coverage_dimensions import fixture as dimensions
    from validate_coverage_buffer_rtv import fixture as buffer_fixture
    from validate_coverage_viewport import fixture as viewport
    from validate_pre_raster_uav import fixture as writer
    from validate_so_passthrough import fixture as passthrough, multi_fixture
    from validate_quad_target_edges import variant

    fixtures = []
    def add(label, create, event=100, unbind=False):
        path = out / (label + '.gpa_frame')
        create(path)
        fixtures.append((label, path, event, unbind))
    for samples in (1, 2, 4, 8):
        for array in (False, True):
            for depth_only in (False, True):
                add(f'target-{samples}-{array}-{depth_only}',
                    lambda path: target_fixture(path, samples=samples, array=array, depth_only=depth_only))
    for dim in ('1d', '3d'):
        for array in (False, True):
            for slots in ((0,), (3,)):
                add(f'dimension-{dim}-{array}-{slots[0]}',
                    lambda path: dimensions(path, dim=dim, array=array, slots=slots), 500)
    for readonly in (False, True):
        for slots in ((), (0,)):
            add(f'depth1d-{readonly}-{len(slots)}',
                lambda path: dimensions(path, depth=True, readonly=readonly, slots=slots), 500)
    add('3d-all-slices', lambda path: dimensions(path, dim='3d', all_w=True), 500)
    for fmt in (28, 42, 41, 2, 49, 61):
        add(f'buffer-{fmt}', lambda path: buffer_fixture(path, fmt=fmt, slots=(0, 3), uavs=1), 500)
    for kind in ('typed', 'counter', 'null'):
        for negative in (False, True):
            add(f'viewport-{kind}-{negative}', lambda path: viewport(path, kind=kind, negative=negative))
    for stage in ('vs', 'hs', 'ds', 'gs'):
        add(f'writer-{stage}', lambda path: writer(path, stage, 63, True))
    for stream in (0, 1, 2, 3, 0xffffffff):
        add(f'stream-{stream}', lambda path: multi_fixture(path, stream), 150)
    for signature in (False, True):
        for tessellated in (False, True):
            add(f'passthrough-{signature}-{tessellated}',
                lambda path: passthrough(path, profile='ds_5_0' if tessellated else 'vs_5_0',
                                         signature_only=signature, tessellated=tessellated), 150)
    add('unbound-output', lambda path: target_fixture(path, array=True), unbind=True)
    seed = out / 'routed-base.gpa_frame'
    target_fixture(seed, array=True)
    add('routed-viewport', lambda path: variant(seed, path, targetless=True))

    jobs, expected, labels = [], [], []
    requests = [dict(target=target) for target in ('auto', 'depth', 'rt0', 'rt3', 'rt7', 'invalid')]
    requests += [dict(target='auto', layer=layer) for layer in (0, 1, 2, 3, 99)]
    for label, path, event_id, unbind in fixtures:
        for driver in ('hardware', 'warp'):
            job = dict(capture=str(path), event=event_id, warp=driver == 'warp', unbind=unbind, requests=requests)
            with Frame(path) as frame:
                device = Device(driver)
                try:
                    engine = Engine(frame, device)
                    engine.replay(until=event_id, before=True, readback=False)
                    event = draw_event(frame, frame.entries[event_id])
                    state = decode_state(frame.payload(event['state_id']))
                    engine.bind(state)
                    result = dict(before=fingerprint_outputs(engine, state), producer=producer(engine, state), selections=[])
                    if unbind:
                        device.call(33, [U, P, P], 0, None, None)
                    for request in requests:
                        row = {}
                        try:
                            selected, metadata = select(engine, state, request['target'], request.get('layer'))
                            width, height = (selected['width'], selected['height']) if selected is not None else extent(device)[:2]
                            row.update(selected=selected, metadata=metadata, size=[width, height], dummy=[])
                            for prepared in (False, True):
                                view = dummy(device, selected, metadata, width, height, prepared)
                                owner = view_resource(device, view)
                                dimension = U()
                                method(owner, 7, None, [P])(owner, c.byref(dimension))
                                count = {1: 6, 2: 8, 3: 11, 4: 9}[dimension.value]
                                desc = (U * count)()
                                method(owner, 10, None, [P])(owner, desc)
                                view_desc = (U * 5)()
                                method(view, 8, None, [P])(view, view_desc)
                                resource = dict(type=0x82 + dimension.value, desc=list(desc))
                                device.inspect_call(50, [P, P], view, (F * 4)(.25, .5, .75, 1))
                                if dimension.value == 1:
                                    staging = device.buffer([desc[0], 3, 0, 0x20000, 0, 0], None)
                                    device.inspect_call(47, [P, P], staging, owner)
                                    mapped = Mapped()
                                    checked(method(device.context, 14, I, [P, U, U, U, P])(
                                        device.context, staging, 0, 1, 0, c.byref(mapped)), 'Read dummy buffer')
                                    try: raw = c.string_at(mapped.data, desc[0])
                                    finally: device.call(15, [P, U], staging, 0)
                                else:
                                    raw = storage_bytes(engine, owner, resource)
                                row['dummy'].append(dict(type=resource['type'], desc=list(desc), view=list(view_desc),
                                                         bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
                        except (ValueError, RuntimeError) as error:
                            row['error'] = str(error)
                        result['selections'].append(row)
                    result['after'] = fingerprint_outputs(engine, state)
                    assert ('error' in result['selections'][0]) == unbind, (label, result['selections'][0])
                finally:
                    device.close()
            jobs.append(job)
            expected.append(result)
            labels.append(driver + '-' + label)
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
    failures = []
    for label, want, got in zip(labels, expected, actual):
        want, got = json.loads(json.dumps(want)), json.loads(json.dumps(got))
        if 'selections' in got:
            for x, y in zip(want['selections'], got['selections']):
                if 'error' in x and 'error' in y:
                    x['error'] = y['error'] = True
        if want != got or want['before'] != want['after'] or got.get('before') != got.get('after'):
            failures.append(dict(label=label, expected=want, actual=got))
    report = dict(passed=not failures, cases=len(jobs), selections=len(jobs)*len(requests),
                  rejected_selections=sum('error' in row for result in expected for row in result['selections']),
                  failures=failures, probe_sha256=hashlib.sha256(a.probe.read_bytes()).hexdigest(),
                  reference_sources={name: hashlib.sha256((a.reference / 'standalone' / name).read_bytes()).hexdigest()
                                     for name in ('quad_targets.py', 'quad_counter.py', 'coverage_fragment.py')})
    (out / 'validation.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v for k, v in report.items() if k != 'failures'}))
    assert not failures, [f['label'] for f in failures[:10]]


if __name__ == '__main__':
    main()
