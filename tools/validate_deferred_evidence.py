"""CPU audit of the pinned M3 producer, capture, API and serial replay evidence.

This audits saved observations; it never establishes traditional-list support.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from validate_compatibility_gate import check_case, digest, load, manifest_digest, require
from validate_corpus import original_rgba


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--validation', type=Path, required=True)
    parser.add_argument('--api-evidence', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--corpus', type=Path, default=Path('docs/deferred-version-corpus.json'))
    parser.add_argument('--gate', type=Path, default=Path('docs/deferred-version-gate.json'))
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    manifest_path = repo/args.corpus
    manifest = load(manifest_path)
    gate = load(repo/args.gate)['suites'][0]
    require(manifest_digest(manifest_path) == gate['manifest_sha256'], 'Changed corpus')
    result = load(args.validation/'validation.json')
    require(result['manifest_sha256'] == digest(manifest_path), 'Wrong validation manifest')
    require(len(result['cases']) == len(manifest['cases']) == 18, 'Incomplete corpus')
    rejected = len(gate['expected_rejections'])
    counts = dict(cases=18, positive_cases=18-rejected, rejected_cases=rejected, producer_frames=0,
                  producer_storage_checks=0, producer_images=0, baseline_readbacks=0,
                  captured_restore_failures=0, original_runs=0, native_positive_runs=0,
                  native_rejection_attempts=0, resource_exports=0, diagnostic_controls=0)
    green = bytes([0, 255, 0, 255])*64
    original_player = '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b'
    for case, spec in zip(result['cases'], manifest['cases']):
        require(case['id'] == spec['id'], 'Reordered or missing case')
        negative = gate['expected_rejections'].get(case['id'])
        check_case(case, spec, negative, args.validation/case['id'], 2)
        if 'expected_unused_cb_intervals' in spec:
            expected = spec['expected_unused_cb_intervals']
            observations = case['preflight']['report']['unused_constant_buffer_lifetimes']
            require(len(observations) == expected and all(o['native_binding_recovered'] is False
                    and o['event_id'] < o['closing_event_id'] for o in observations), 'Preflight CB provenance')
            for run in case['native']:
                lifetimes = run['report']['unused_constant_buffer_lifetimes']
                require(len(lifetimes) == expected and
                        run['report']['counts'].get('unmaterialized_constant_buffer_setters', 0) == expected,
                        'Missing CB execution provenance')
                for observed, native in zip(observations, lifetimes):
                    require(native['native_binding_recovered'] is False and
                            int(native['event']) == observed['event_id'] and
                            int(native['closing_event']) == observed['closing_event_id'] and
                            native['stage'] == observed['stage'] and
                            list(map(int, native['resources'])) == observed['resource_ids'], 'CB provenance disagrees')
        capture = args.artifacts/spec['path']
        require(digest(capture) == spec['sha256'], 'Capture changed')
        require(case['original_status'] == 'repeat_stable' and len(case['original']) == 2,
                'Missing repeated original replay')
        for index, run in enumerate(case['original'], 1):
            require(run['exit_code'] == 0 and not run['timed_out'] and run['kernel_completed'] and
                    run['report']['replayed_rgba_sha256'] == hashlib.sha256(green).hexdigest() and
                    run['report']['native']['player_sha256'] == original_player,
                    'Original replay mismatch')
            require(original_rgba(args.validation/case['id']/f'original-{index}') == green,
                    'Original raw pixels changed')
            counts['original_runs'] += 1
        if not negative:
            for index in [1, 2]:
                require((args.validation/case['id']/f'native-{index}'/'frame.rgba').read_bytes() == green,
                        'Native raw pixels changed')
        for boundary in case.get('boundaries', []):
            for index in [1, 2]:
                raw = args.validation/case['id']/f"boundary-{boundary['resource']}-{boundary['event']}-{index}"/'buffer.bin'
                require(digest(raw) == boundary['expected_storage_sha256'], 'Exported buffer changed')
        counts['native_rejection_attempts' if negative else 'native_positive_runs'] += 2
        counts['resource_exports'] += sum(len(b['runs']) for b in case.get('boundaries', []))
        counts['diagnostic_controls'] += sum(len(c['runs']) for c in case.get('controls', []))

    for group, complete in [('m3-deferred-version-originals', False),
                            ('m3-deferred-version-complete', True)]:
        root = args.artifacts/group
        producer = load(root/'manifest.json')
        require(producer['completed'] and producer['native_baselines_passed'] and
                producer['delivered_captures'] == 9, 'Incomplete producer')
        for name, expected in producer['producer_files'].items():
            require(digest(root/'producer'/name) == expected, 'Changed frozen producer')
        require(producer['original_dlls']['shimd3d64.dll'] ==
                'cb0b99d113511cbf7ca9f949d97aa02e4a1f8f110b1f3ad3165d8a81035dee31',
                'Wrong capture shim')
        require([c['mode'] for c in producer['cases']] == list(range(9)), 'Missing producer mode')
        for c in producer['cases']:
            mode = c['mode']
            restore = mode in [3, 4, 7, 8]
            directory = root/str(mode)
            require(digest(directory/'capture.gpa_frame') == c['capture_sha256'], 'Capture provenance')
            for kind in ['native', 'captured']:
                run = c['runs'][kind]
                require(run['exit_code'] == 0 and not run['timed_out'] and
                        not run.get('validation_error'), 'Producer process failed')
                output = directory/kind
                for name, expected in run['files'].items():
                    require(digest(output/name) == expected, 'Producer output changed')
                oracle = load(output/'oracle.json')
                require(oracle['completed'] and [f['frame'] for f in oracle['frames']] == list(range(12)),
                        'Incomplete producer frames')
                require(not oracle['storage_failures'] and not oracle['image_failures'] and
                        not oracle['finish_state_failures'], 'Producer data/Finish mismatch')
                for frame in oracle['frames']:
                    number = frame['frame']
                    expected_failure = 3 if kind == 'captured' and restore and number == 6 else 0
                    require(frame['execute_state_failures'] == expected_failure and
                            frame['finish_state_failures'] == 0, 'State observation changed')
                    counts['captured_restore_failures'] += expected_failure
                    require([s['step'] for s in frame['steps']] == [0, 1, 2], 'Missing step')
                    for step, expected in enumerate([(3, 1, 2, 3), (22, 2, 5, 5), (78, 3, 11, 3)]):
                        raw = (output/f'frame-{number}-step-{step}.bin').read_bytes()
                        observation = frame['steps'][step]
                        require(raw == struct.pack('<4I', *expected) and observation['verified'] and
                                observation['actual'] == observation['expected'] == list(expected),
                                'Resource version/order oracle failed')
                        counts['producer_storage_checks'] += 1
                    require((output/f'frame-{number}.rgba').read_bytes() == green and
                            frame['image_verified'], 'Producer image oracle failed')
                    counts['producer_images'] += 1
                    counts['producer_frames'] += 1
                    if complete:
                        require((output/f'frame-{number}-baseline.bin').read_bytes() ==
                                struct.pack('<4I', 101, 103, 107, 109), 'Sentinel readback failed')
                        counts['baseline_readbacks'] += 1
            api = args.api_evidence/group/str(mode)
            lists = load(api/'command-lists/command-lists.json')
            require(not lists['command_lists'] and not lists['execute_commands'] and
                    not lists['execution_supported'], 'Unexpected traditional-list evidence')
            commands = load(api/'commands/commands.json')['commands']
            require(all(c['status'] == 'decoded' for c in commands), 'Undecoded API record')
            require(sum(c['name'] == 'Dispatch' for c in commands) == 3, 'Dispatch count changed')
            if restore:
                getters = [c for c in commands if c['name'] in ['CSGetShader', 'CSGetConstantBuffers']]
                require(len(getters) == 6, 'Missing saved post-Execute getters')
                # Capture-frame expansion leaves the list's shader/CB bound, corroborating
                # the native pointer comparisons in the injected producer (not replay state).
                baseline = next(c for c in commands if c['name'] == 'CSSetConstantBuffers')
                baseline_id = next(f['value'] for f in baseline['fields'] if f['name'] == 'bindings[0]')
                for getter in getters:
                    key = 'returned_shader' if getter['name'] == 'CSGetShader' else 'returned_objects[0]'
                    value = next(f['value'] for f in getter['fields'] if f['name'] == key)
                    require(value != 0 and (key == 'returned_shader' or value != baseline_id),
                            'Saved getter no longer corroborates capture-side restoration loss')
    summary = dict(schema='FloraGPA deferred evidence audit 1', completed=True, passed=True,
                   counts=counts, exe_sha256=result['exe_sha256'],
                   corpus_sha256=digest(manifest_path), validation_sha256=digest(args.validation/'validation.json'),
                   limits=['Expanded immediate streams only; traditional lists remain unsupported',
                           'Original-player adapter/configuration equivalence is unproven',
                           'Unmaterialized sentinel bindings never establish native binding recovery',
                           'Capture-side restore observations do not assert every original capture behaves this way'])
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(summary, stream, indent=2)
        stream.write('\n')
    print(json.dumps(counts))


if __name__ == '__main__':
    main()
