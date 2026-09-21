"""Compare real private Quad UAV passes, class instances and prepared depth."""
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
    parser = argparse.ArgumentParser()
    for name in ('reference', 'probe', 'qt-bin', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    a = parser.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'reference').mkdir()
    sys.path[:0] = [str(a.reference / 'standalone'), str(a.reference / 'tools')]
    from dx11 import Device, P, U, I, F, Mapped, method, checked
    from frame import Frame
    from engine import Engine
    from events import draw_event, execution_parameters
    from state import decode_state
    from pre_raster_uav import PrivateGraphicsUAVs, writers
    from quad_counter import fingerprint_outputs, preserve_high_uavs
    from quad_targets import select, dummy, storage_bytes
    from quad_depth import prepare
    from coverage_isolated import output_copies, view_resource
    from coverage_viewport import extent
    from shaders import compile_hlsl
    from experiments import blob
    from validate_buffer_edits import Fixture, experiment
    from validate_pre_raster_uav import fixture as writer
    from validate_coverage_viewport import fixture as viewport
    from validate_so_passthrough import fixture as passthrough, HULL
    from validate_class_linkage import fixture as class_fixture, COMMON, V, HULL as CLASS_HULL, DOMAIN

    def rewrite(path, shader_sources=None, extra_uav=False):
        output = Fixture(); output.records = []
        with Frame(path) as frame:
            programs = {frame.resource(id)['data_id']: compile_hlsl(source, stage + '_5_0')[0]
                        for id, (stage, source) in (shader_sources or {}).items()}
            for entry in frame.entries.values():
                raw = frame.payload(entry.id)
                if entry.id in programs:
                    code = programs[entry.id]; raw = struct.pack('<Q', len(code)) + code + bytes(8)
                if extra_uav and entry.id in (99, 199):
                    raw = bytearray(raw)
                    struct.pack_into('<Q', raw, 17428, 702)
                    struct.pack_into('<I', raw, 17484, 1)
                    struct.pack_into('<I', raw, 17528, 2)
                output.add(entry.id, entry.category, entry.type, raw)
        if extra_uav:
            output.buffer(700, [256, 0, 128, 0, 32, 0], bytes(256))
            output.view(702, 'uav', 700, [39, 1, 0, 64, 1])
        output.write(path)

    fixtures = []
    def add(label, create, event=100, edits=None, rejected=False, **options):
        path = out / (label + '.gpa_frame'); create(path)
        fixtures.append((label, dict(capture=str(path), event=event, **options), edits, rejected))
    for stage in ('vs', 'hs', 'ds', 'gs'):
        for slot in (0, 1, 4, 7, 8, 63):
            for counter in (False, True):
                add(f'writer-{stage}-{slot}-{counter}', lambda path: writer(path, stage, slot, counter))
    for kind in ('typed', 'counter'):
        for slots in ((0,), (0, 4, 7), tuple(range(8))):
            add(f'pixel-{kind}-{len(slots)}', lambda path: viewport(path, kind=kind, slots=slots))
    for signature in (False, True):
        add('passthrough-' + str(signature), lambda path: passthrough(path, signature_only=signature))

    bodies = dict(ps='float4 main():SV_Target{return selected.apply(0);}',
        vs='float4 main(float2 p:POSITION):SV_Position{return selected.apply(float4(p,0,1));}',
        gs=V+'[maxvertexcount(3)]void main(triangle V input[3],inout TriangleStream<V> output){for(uint i=0;i<3;i++){V v;v.p=selected.apply(input[i].p);output.Append(v);}}',
        hs=CLASS_HULL.replace('EXPR', 'selected.apply(input[id].p)'),
        ds=DOMAIN.replace('EXPR', 'selected.apply(p)'))
    def dynamic(path, stage, created):
        source = 'RWByteAddressBuffer data:register(u1);' + COMMON.replace('return x+value;', 'data.Store(0,123);return x+value;') + bodies[stage]
        class_fixture(path, stage=stage, created=created, source=source)
        rewrite(path, extra_uav=True)
    for stage in bodies:
        for created in (False, True):
            add(f'class-{stage}-{created}', lambda path: dynamic(path, stage, created))

    for slot in (0, 1, 4, 7, 63):
        for counter in (False, True):
            def shared(path):
                writer(path, 'vs', slot, counter)
                decl = (f'RWStructuredBuffer<uint> data:register(u{slot});' if counter else f'RWByteAddressBuffer data:register(u{slot});')
                action = 'uint n=data.IncrementCounter();data[n]=100;' if counter else 'data.Store(240,999);'
                body = 'void main(){' + action + '}' if slot == 0 else 'float4 main():SV_Target{' + action + 'return float4(1,0,0,1);}'
                rewrite(path, {12: ('ps', decl + body)})
            add(f'shared-vs-ps-{slot}-{counter}', shared)
    def hull_domain(path):
        writer(path, 'ds', 1, False)
        source = 'RWByteAddressBuffer data:register(u1);' + HULL.replace('return input[i];', 'data.Store(12,77);return input[i];')
        rewrite(path, {80: ('hs', source)})
    add('shared-hs-ds', hull_domain)
    def exhausted(path, pixel=False):
        writer(path, 'vs', 0, False)
        decl = ''.join(f'RWByteAddressBuffer b{i}:register(u{i});' for i in range(64))
        loads = '+'.join(f'b{i}.Load(0)' for i in range(64))
        if pixel:
            rewrite(path, {10: ('vs', 'float4 main(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,.5,1);}'),
                           12: ('ps', decl + 'void main(){uint value=' + loads + ';b0.Store(0,value);}')})
        else:
            rewrite(path, {10: ('vs', decl + 'float4 main(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,(' + loads + ')*.00001,1);}')})
    add('exhausted-slots', exhausted, rejected=True)
    add('pixel-only-exhausted-slots', lambda path: exhausted(path, True), rejected=True)
    add('failure-high', lambda path: writer(path, 'gs', 63, True), rejected=True, **{'throw': True})
    add('disabled', lambda path: writer(path, 'vs', 1, True), edits=[dict(kind='enabled', event=100, value=False)])
    edited = out / 'edited-vs.dxbc'
    edited.write_bytes(compile_hlsl('RWStructuredBuffer<uint> data:register(u1);float4 main(uint id:SV_VertexID):SV_Position{uint n=data.IncrementCounter();data[n]=123;return float4(id==2?3:-1,id==1?3:-1,.625,1);}', 'vs_5_0')[0])
    add('edited-writer', lambda path: writer(path, 'vs', 1, True), edits=[dict(kind='shader', resource=10, asset=blob(edited))])

    def outputs(device):
        rt, depth, so = (P*8)(), P(), (P*4)()
        device.call(89, [U, P, P], 8, rt, c.byref(depth))
        uav = (P*(64 if device.feature_level >= 0xb100 else 8))()
        device.call(90, [U, P, P, U, U, P], 0, None, None, 0, len(uav), uav)
        device.call(93, [U, P], 4, so)
        values = [*rt, depth.value, *uav, *so]
        for value in values:
            if value: method(value, 2, U, [])(value)
        return values
    def classes(device):
        result = []
        for slot in (76, 98, 102, 82, 74):
            shader, instances, count = P(), (P*256)(), U(256)
            device.call(slot, [P, P, P], c.byref(shader), instances, c.byref(count))
            result.append(list(instances[:count.value]))
            for obj in [shader.value, *result[-1]]:
                if obj: method(obj, 2, U, [])(obj)
        return result
    def buffer_bytes(device, obj, size):
        staging = device.buffer([size, 3, 0, 0x20000, 0, 0], None)
        device.inspect_call(47, [P, P], staging, obj)
        mapped = Mapped()
        checked(method(device.context, 14, I, [P, U, U, U, P])(device.context, staging, 0, 1, 0, c.byref(mapped)), 'Read Quad test storage')
        try: return c.string_at(mapped.data, size)
        finally: device.call(15, [P, U], staging, 0)
    def dummy_digest(engine, view):
        device = engine.device
        owner = view_resource(device, view); dimension = U()
        method(owner, 7, None, [P])(owner, c.byref(dimension))
        desc = (U*{1: 6, 2: 8, 3: 11, 4: 9}[dimension.value])()
        method(owner, 10, None, [P])(owner, desc)
        raw = buffer_bytes(device, owner, desc[0]) if dimension.value == 1 else storage_bytes(engine, owner, dict(type=0x82+dimension.value, desc=list(desc)))
        return hashlib.sha256(raw).hexdigest()

    marker_code = compile_hlsl('float4 main():SV_Target{return float4(.25,.5,.75,1);}', 'ps_5_0')[0]
    jobs, expected, labels = [], [], []
    for label, base_job, edits, rejected in fixtures:
        for driver in ('hardware', 'warp'):
            job = dict(base_job, warp=driver == 'warp')
            with Frame(Path(job['capture'])) as frame:
                project = None
                if edits:
                    job['experiment'] = str(out / (label+'-'+driver+'-experiment.json'))
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
                        previous_outputs, previous_classes, counts = outputs(device), classes(device), dict(engine.counts)
                        def submit():
                            if project and not project.events.get(event['id'], {}).get('enabled', True): return 0
                            params = execution_parameters(frame, event, engine)
                            args, types = list(params.values()), [I if name == 'base_vertex' else U for name in params]
                            if 'argument_buffer' in params: types[0] = P; args[0] = engine.resources.get(args[0])
                            device.call(event['slot'], types, *args)
                            return 1
                        try:
                            result['writers'] = writers(engine, state)
                            helper = PrivateGraphicsUAVs(engine, state)
                            result['metadata'] = helper.report()
                            selected, metadata = select(engine, state)
                            width, height = (selected['width'], selected['height']) if selected else extent(device)[:2]
                            marker = device.shader('ps', marker_code)
                            buffers, views = [], []
                            for i in range(4):
                                obj = device.buffer([16, 0, 128, 0, 0, 0], struct.pack('<4I', *([i+11]*4)))
                                buffers.append(obj); views.append(device.view('uav', obj, struct.pack('<5I', 42, 1, 0, 4, 0)))
                            result['passes'] = []
                            for preserve in (False, True, False, True):
                                with preserve_high_uavs(device, state), output_copies(engine, event, state):
                                    engine.bind(state)
                                    if project: project.pipeline(engine, event, state)
                                    row = dict(before=fingerprint_outputs(engine, state))
                                    target = dummy(device, selected, metadata, width, height)
                                    device.inspect_call(50, [P, P], target, (F*4)(0, 0, 0, 0))
                                    depth = P(); device.call(89, [U, P, P], 0, None, c.byref(depth))
                                    try: device.call(34, [U, P, P, U, U, P, P], 1, (P*1)(target), depth, 1, 4, (P*4)(*views), None)
                                    finally:
                                        if depth.value: method(depth.value, 2, U, [])(depth.value)
                                    if not preserve: device.call(9, [P, P, U], marker, None, 0)
                                    helper.bind(preserve_ps=preserve)
                                    actual = (P*4)(); device.call(90, [U, P, P, U, U, P], 0, None, None, 1, 4, actual)
                                    row['reserved_bound'] = list(actual) == views
                                    for obj in actual:
                                        if obj: method(obj, 2, U, [])(obj)
                                    row['classes_preserved'] = classes(device)[:5 if preserve else 4] == previous_classes[:5 if preserve else 4]
                                    row['submissions'] = submit()
                                    row['after'] = fingerprint_outputs(engine, state)
                                    row['dummy_sha256'] = dummy_digest(engine, target)
                                    row['reserved_sha256'] = [hashlib.sha256(buffer_bytes(device, obj, 16)).hexdigest() for obj in buffers]
                                    if job.get('throw'): raise RuntimeError('Injected private Quad pass failure')
                                result['passes'].append(row)
                            def prepare_submit():
                                helper.bind(preserve_ps=True)
                                return submit()
                            _, report, _ = prepare(engine, event, state, width, height, prepare_submit, preserve_high_uavs,
                                selected=selected, target_metadata=metadata)
                            report.update(color_and_uav_outputs_removed=False, private_uavs_preserved_for_pre_raster_stages=True)
                            result['prepared'] = report
                        except (ValueError, RuntimeError) as error: result['error'] = str(error)
                        result.update(after=fingerprint_outputs(engine, state), bindings_restored=outputs(device)==previous_outputs,
                                      classes_restored=classes(device)==previous_classes, counts_unchanged=dict(engine.counts)==counts)
                        (out/'reference'/(driver+'-'+label+'.json')).write_text(json.dumps(result, indent=2))
                        assert ('error' in result) == rejected, (driver, label, result.get('error'))
                        if label == 'pixel-only-exhausted-slots': assert result['writers'] == [], label
                        assert result['before']==result['after'] and all(result[key] for key in ('bindings_restored', 'classes_restored', 'counts_unchanged')), label
                        if not rejected:
                            assert result['passes'][0] == result['passes'][2] and result['passes'][1] == result['passes'][3], label
                            assert all(row['reserved_bound'] and row['classes_preserved'] and row['before']==result['before'] for row in result['passes']), label
                            seeds = [hashlib.sha256(struct.pack('<4I', *([i+11]*4))).hexdigest() for i in range(4)]
                            assert all(row['reserved_sha256']==seeds for row in result['passes']), label
                            if label.startswith('writer-'):
                                assert all(row['before']['302'] != row['after']['302'] for row in result['passes']), label
                finally: device.close()
            jobs.append(job); expected.append(result); labels.append(driver+'-'+label)
            (out/'progress.json').write_text(json.dumps(dict(cases=len(jobs), last=labels[-1])))
    (out/'jobs.json').write_text(json.dumps(jobs))
    (out/'expected.json').write_text(json.dumps(expected, indent=2))
    env = {k:v for k,v in os.environ.items() if k.upper() in ('SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA')}
    env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + str(Path(os.environ['SystemRoot'])/'System32')
    run = subprocess.run([str(a.probe.resolve()), '--probe', str(out/'jobs.json'), str(out/'actual.json')], env=env, capture_output=True, timeout=300)
    (out/'native.log').write_bytes(run.stdout+run.stderr)
    assert run.returncode == 0, run.stderr
    actual = json.loads((out/'actual.json').read_text())
    assert len(actual)==len(expected)
    failures = [dict(label=label, expected=want, actual=got) for label,want,got in zip(labels,expected,actual) if want!=got]
    report = dict(passed=not failures, cases=len(jobs), rejected=sum('error' in row for row in expected), failures=failures,
                  probe_sha256=hashlib.sha256(a.probe.read_bytes()).hexdigest(), reference_sources={name:hashlib.sha256((a.reference/'standalone'/name).read_bytes()).hexdigest() for name in ('pre_raster_uav.py','quad_depth.py','coverage_isolated.py')})
    (out/'validation.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k:v for k,v in report.items() if k!='failures'}))
    assert not failures, [row['label'] for row in failures[:10]]


if __name__ == '__main__': main()
