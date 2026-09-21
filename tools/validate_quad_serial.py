"""Compare native Quad serial submissions with the original Python GPU helper."""
import argparse
from contextlib import nullcontext
import hashlib
import itertools
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--qt-bin', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sys.path[:0] = [str(args.reference / 'standalone'), str(args.reference / 'tools')]
    from frame import Frame
    from engine import Engine
    from dx11 import Device
    from state import decode_state
    from events import draw_event
    from shaders import compile_hlsl
    from quad_serial import SerialCounter, expand_strip
    from quad_strip_fixtures import fixture
    from validate_quad_linear import fixture as linear_fixture
    from validate_class_linkage import clone, fixture as class_fixture
    from validate_buffer_edits import experiment, operation, Fixture
    from experiments import blob
    from validate_stream_output import fixture as so_fixture

    jobs, expected, labels = [], [], []
    for factor in (2, 3):
        for length in range(7):
            for values in itertools.product((0, 1, 0xffffffff), repeat=length):
                indices, metadata = expand_strip(values, 0xffffffff, factor)
                jobs.append(dict(action='expand', values=values, cut=0xffffffff, factor=factor))
                expected.append(dict(indices=indices, metadata=metadata))
                labels.append(f'expand-{factor}-{values}')

    fixtures = []
    configs = []
    for fmt in (None, 57, 42):
        configs += [dict(fmt=fmt, cull=cull, front=front) for cull in (1, 2, 3) for front in (0, 1)]
        configs += [dict(fmt=fmt, pattern=pattern) for pattern in (('short', 'zero') if fmt is None else ('restart', 'degenerate', 'cuts', 'short', 'zero'))]
        configs += [dict(fmt=fmt, instances=0), dict(fmt=fmt, indirect=True)]
    for n, config in enumerate(configs):
        path = out / f'strip-{n}.gpa_frame'
        fixture(path, **config)
        fixtures.append((path.stem, path, None))
    for top in (1, 2, 3):
        configs = [dict(topology=top, fmt=fmt, indirect=indirect) for fmt in (None, 57, 42) for indirect in (False, True)]
        configs += [dict(topology=top, **extra) for extra in (dict(pattern='zero'), dict(pattern='short'), dict(instances=0))]
        if top == 3:
            configs += [dict(topology=top, fmt=fmt, pattern=pattern) for fmt in (57, 42) for pattern in ('restart', 'cuts')]
        for n, config in enumerate(configs):
            path = out / f'linear-{top}-{n}.gpa_frame'
            linear_fixture(path, **config)
            fixtures.append((path.stem, path, None))

    # Encode instance-data fetch alongside system IDs in the original PS color.
    # The same frame also exercises captured layout parsing and nonzero VB offsets.
    seed = out / 'instances-source.gpa_frame'
    fixture(seed, fmt=57, pattern='restart', instances=5)
    source = ('struct V{float4 p:SV_Position;nointerpolation uint id:TEXCOORD0;};'
              'V main(uint v:SV_VertexID,uint i:SV_InstanceID,uint a:INST){V o;uint k=v-2;'
              'o.p=float4(-1+(k/2)*(2.0/3.0),(k&1)?-1:1,.5,1);o.id=(a*3+v+i*7)%64;return o;}')
    code = compile_hlsl(source, 'vs_5_0')[0]
    for step in (0, 1, 2, 3):
        for overflow in (False, True):
            path = out / f'instances-{step}-{overflow}.gpa_frame'
            def change(entry, raw):
                if entry.id == 11:
                    return struct.pack('<Q', len(code)) + code + bytes(8)
                if entry.id in (99, 199):
                    raw = bytearray(raw)
                    struct.pack_into('<Q', raw, 144, 3000)
                    struct.pack_into('<Q', raw, 164, 3010)
                    struct.pack_into('<I', raw, 416, 4)
                    struct.pack_into('<I', raw, 544, 0xfffffffc if overflow else 4)
                return raw
            def extra(f):
                f.add(3000, 5, 0x82, bytes(16) + struct.pack('<Q', 3001))
                f.data(3002, b'INST\0')
                f.add(3001, 9, 0x84, struct.pack('<IQ6II', 1, 3002, 0, 42, 1, 0, 1, step, len(code)) + code)
                f.buffer(3010, [128, 0, 1, 0, 0, 0], struct.pack('<32I', *range(32)))
            clone(seed, path, change, extra)
            fixtures.append((path.stem, path, None))

    for fmt in (57, 42):
        path = out / f'edited-source-{fmt}.gpa_frame'
        fixture(path, fmt=fmt, pattern='restart')
        values = struct.pack('<' + ('H' if fmt == 57 else 'I') * 5, *([0xffff if fmt == 57 else 0xffffffff] * 2 + [2, 3, 4]))
        operations = [operation(20, 0, values)]
        fixtures.append((f'edited-indices-{fmt}', path, operations))
        # A copy before the draw proves that index assembly reads native storage.
        def upload(f):
            with Frame(path) as frame:
                size = frame.resource(20)['desc'][0]
            raw = values + bytes(size - len(values))
            f.buffer(4000, [size, 0, 0, 0, 0, 0], raw)
            f.add(80, 7, 0x3e, struct.pack('<4Q', 0, 1, 20, 4000))
        changed = clone(path, out / f'gpu-indices-{fmt}.gpa_frame', lambda e, raw: raw, upload)
        fixtures.append((f'gpu-indices-{fmt}', changed, None))
        def invalid(entry, raw):
            if entry.id in (100, 200):
                raw = bytearray(raw)
                struct.pack_into('<I', raw, 24, 0xffffffff)
            return raw
        bad = clone(path, out / f'invalid-range-{fmt}.gpa_frame', invalid)
        fixtures.append((f'invalid-range-{fmt}', bad, None))
    for created in (False, True):
        path = out / f'class-vs-{created}.gpa_frame'
        class_fixture(path, stage='vs', created=created)
        fixtures.append((path.stem, path, None))

    for fmt in (None, 57, 42):
        source_path = out / f'direct-source-{fmt}.gpa_frame'
        fixture(source_path, fmt=fmt)
        for top in (4, 5):
            path = out / f'direct-{fmt}-{top}.gpa_frame'
            copy = Fixture()
            copy.records = []
            with Frame(source_path) as frame:
                for entry in frame.entries.values():
                    raw, kind = frame.payload(entry.id), entry.type
                    if entry.id in (99, 199):
                        raw = bytearray(raw)
                        struct.pack_into('<I', raw, 152, top)
                    if entry.id in (100, 200):
                        if fmt:
                            count, _, start, base, _ = struct.unpack_from('<IIIiI', raw, 24)
                            kind, raw = 0x39, raw[:24] + struct.pack('<IIi', count, start, base)
                        else:
                            count, _, start, _ = struct.unpack_from('<4I', raw, 24)
                            kind, raw = 0x37, raw[:24] + struct.pack('<II', count, start)
                    copy.add(entry.id, entry.category, kind, raw)
            copy.write(path)
            fixtures.append((path.stem, path, None))

    replacement = out / 'replacement.dxbc'
    replacement.write_bytes(compile_hlsl(source.replace('(a*3+v+i*7)%64', '(a+v*3+i*5)%64'), 'vs_5_0')[0])
    fixtures.append(('replacement-vs', out / 'instances-2-False.gpa_frame',
                     [dict(kind='shader', resource=10, asset=blob(replacement))]))

    # Resolve DrawAuto from actual preceding SO writes, including paired and
    # padded records. The selected draw has no GS/SO and uses the serial path.
    for label, config in (
        ('interleaved', {}), ('paired', dict(paired=True)),
        ('gap', dict(gap=True)), ('append', dict(append=True)),
        ('implicit', dict(implicit_stride=True)), ('stale-count', dict(auto_count=0)),
    ):
        path = out / f'auto-{label}.gpa_frame'
        so_fixture(path, auto_gs=0, **config)
        fixtures.append((path.stem, path, None))

    for label, path, operations in fixtures:
        for driver in ('hardware', 'warp'):
            automatic = label.startswith('auto-')
            event_id = 200 if automatic else 100
            textures = [50] if automatic else [30]
            buffers = ([40, 42] if label == 'auto-paired' else [40]) if automatic else []
            job = dict(capture=str(path), event=event_id, warp=driver == 'warp', textures=textures, buffers=buffers)
            with Frame(path) as frame:
                project = None
                if operations:
                    job['experiment'] = str(out / f'{label}-{driver}-experiment.json')
                    project = experiment(frame, Path(job['experiment']), operations)
                device = Device(driver)
                try:
                    engine = Engine(frame, device, experiment=project)
                    event = draw_event(frame, frame.entries[event_id])
                    state = decode_state(frame.payload(event['state_id']))
                    engine.replay(until=event_id, before=True, readback=False)
                    with project.inputs(engine, event) if project else nullcontext():
                        engine.bind(state)
                        if project:
                            project.pipeline(engine, event, state)
                        try:
                            helper = SerialCounter(engine, event, state)
                            value = dict(submissions=helper.submit(), metadata=helper.metadata())
                        except (ValueError, RuntimeError) as error:
                            value = dict(error=str(error))
                        value['textures'] = {str(id): hashlib.sha256(engine.read_texture(id)).hexdigest() for id in textures}
                        value['buffers'] = {str(id): hashlib.sha256(engine.read_buffer(id)).hexdigest() for id in buffers}
                    value['counts'] = dict(engine.counts)
                finally:
                    device.close()
            jobs.append(job)
            expected.append(value)
            labels.append(f'{driver}-{label}')
            (out / 'progress.json').write_text(json.dumps(dict(cases=len(expected), last=labels[-1])))

    (out / 'jobs.json').write_text(json.dumps(jobs))
    (out / 'expected.json').write_text(json.dumps(expected, indent=2))
    env = dict(os.environ)
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + str(Path(os.environ['SystemRoot']) / 'System32')
    run = subprocess.run([str(args.probe.resolve()), '--probe', str(out / 'jobs.json'), str(out / 'actual.json')],
                         env=env, capture_output=True, timeout=300)
    (out / 'native.log').write_bytes(run.stdout + run.stderr)
    assert run.returncode == 0, run.stderr
    actual = json.loads((out / 'actual.json').read_text())
    assert len(actual) == len(expected)
    failures = []
    for n, (want, got, job) in enumerate(zip(expected, actual, jobs)):
        if job.get('action') == 'expand':
            ok = want == got
        else:
            want = dict(want)
            got = dict(got)
            ok = got.pop('bindings_restored', False)
            # Python submit() increments both <name>_records and <name>; native
            # Replay exposes execution counts. Verify the redundant record
            # counters before omitting them (these fixtures disable no events).
            def counts(values):
                result = {}
                for key, value in values.items():
                    if key.endswith('_records') and key.startswith(('Draw', 'Dispatch')):
                        assert value == values.get(key[:-8]), (labels[n], key, values)
                    elif not key.startswith(('meta_', 'inspection_')):
                        result[key] = value
                return result
            if 'counts' in got:
                got['counts'] = counts(got['counts'])
                want['counts'] = counts(want['counts'])
            ok &= want == got
        if not ok:
            failures.append(dict(index=n, label=labels[n], expected=want, actual=got))
    report = dict(passed=not failures, comparisons=len(jobs), gpu_cases=len(fixtures)*2,
                  expected_rejections=sum('error' in r for r in expected), failures=failures,
                  probe_sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest(),
                  reference_sources={name: hashlib.sha256((args.reference / 'standalone' / name).read_bytes()).hexdigest()
                                     for name in ('quad_serial.py', 'dxbc_system_id.py', 'geometry.py', 'engine.py')})
    (out / 'validation.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v for k, v in report.items() if k != 'failures'}))
    assert not failures, [(f['index'], f['label']) for f in failures[:10]]


if __name__ == '__main__':
    main()
