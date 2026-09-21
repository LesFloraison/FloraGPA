"""Run original GPU fixtures using native system-ID and Quad array transforms.

This validates the C++ shader transforms, not a migrated Quad executor or UI.
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
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--qt-bin', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--suite', choices=['identities', 'targets', 'strips'], required=True)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path[:0] = [str(args.reference / 'standalone'), str(args.reference / 'tools')]
    import dxbc_system_id
    import quad_targets
    env = dict(os.environ)
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + str(Path(os.environ['SystemRoot']) / 'System32')
    cache, calls = {}, []

    def native(raw, action, **options):
        job = dict(input=base64.b64encode(raw).decode(), action=action, **options)
        key = json.dumps(job, sort_keys=True)
        if key in cache:
            return cache[key]
        index = len(cache)
        input_path = args.out / f'native-{index}.json'
        output_path = args.out / f'native-{index}-result.json'
        input_path.write_text(json.dumps([job]))
        run = subprocess.run([str(args.probe.resolve()), '--probe', str(input_path), str(output_path)],
                             env=env, capture_output=True, timeout=60)
        assert run.returncode == 0, run.stderr
        result = json.loads(output_path.read_text())[0]
        if 'error' in result:
            raise ValueError(result['error'])
        result['output'] = base64.b64decode(result['output'])
        cache[key] = result
        calls.append(dict(action=action, input_sha256=hashlib.sha256(raw).hexdigest(),
                          output_sha256=hashlib.sha256(result['output']).hexdigest()))
        return result

    def offset(raw, value, system):
        result = native(raw, 'offset', offset=value, system=system)
        return result['output'], result['metadata']

    dxbc_system_id.offset_system_id = offset
    dxbc_system_id.offset_instance_id = lambda raw, value: offset(raw, value, 8)
    dxbc_system_id.offset_vertex_id = lambda raw, value: offset(raw, value, 6)
    quad_targets.filter_shader = lambda raw, index, source: native(raw, 'filter', index=index, source=source)['output']
    target = args.out / 'gpu'
    if args.suite == 'identities':
        module = importlib.import_module('validate_quad_system_ids')
        sys.argv = [module.__file__, str(target)]
        module.main()
        rows = json.loads((target / 'results.json').read_text())['results']
        checks = [dict(name=f'identity-{i}', passed=True, equal=row['equal']) for i, row in enumerate(rows)]
    elif args.suite == 'targets':
        module = importlib.import_module('validate_quad_targets')
        sys.argv = [module.__file__, '--out', str(target)]
        module.main()
        checks = json.loads((target / 'validation.json').read_text())['checks']
    else:
        # Direct fixture use avoids the original script's Python CLI/Tk checks,
        # whose child interpreter would bypass the injected native transformer.
        from frame import Frame
        from dx11 import Device
        from engine import Engine
        from state import decode_state
        from events import draw_event
        from quad_serial import SerialCounter
        from quad_strip_fixtures import fixture
        from validate_quad_linear import fixture as linear_fixture
        target.mkdir()
        checks = []
        configs = [(dict(fmt=fmt, pattern=pattern), False)
                   for fmt in (None, 57, 42)
                   for pattern in (('ordinary', 'short', 'zero') if fmt is None else ('ordinary', 'restart', 'degenerate', 'cuts', 'short', 'zero'))]
        configs += [(dict(fmt=fmt, indirect=True), False) for fmt in (None, 57, 42)]
        configs += [(dict(fmt=fmt, topology=topology, pattern=pattern), True)
                    for fmt in (None, 57, 42) for topology in (1, 2, 3)
                    for pattern in (('ordinary', 'zero') if topology != 3 or fmt is None else ('ordinary', 'restart', 'cuts', 'zero'))]
        for n, (options, linear) in enumerate(configs):
            path = target / f'fixture-{n}.gpa_frame'
            (linear_fixture if linear else fixture)(path, **options)
            for driver in ('hardware', 'warp'):
                with Frame(path) as frame:
                    device = Device(driver)
                    try:
                        engine = Engine(frame, device)
                        event = draw_event(frame, frame.entries[100])
                        state = decode_state(frame.payload(event['state_id']))
                        engine.replay(until=100, readback=False)
                        expected = engine.read_texture(30)
                        engine.replay(until=100, before=True, readback=False)
                        engine.bind(state)
                        counter = SerialCounter(engine, event, state)
                        count = counter.submit()
                        actual = engine.read_texture(30)
                        assert actual == expected, (n, driver, options)
                        assert count == counter.planned_submissions
                        checks.append(dict(name=f'{driver}-{n}', passed=True, submissions=count,
                                           options=options, serialization=counter.metadata()))
                    finally:
                        device.close()
            (args.out / 'progress.json').write_text(json.dumps(checks, indent=2))
    assert calls and checks and all(row['passed'] for row in checks)
    report = dict(passed=True, suite=args.suite, checks=checks, native_transformations=calls,
                  probe_sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest())
    (args.out / 'validation.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(dict(passed=True, suite=args.suite, checks=len(checks), native_transformations=len(calls))))


if __name__ == '__main__':
    main()
