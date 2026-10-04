"""Audit merged-list evidence; a faithful replay never repairs a faulty capture.

Checks saved evidence first, then runs explicit buffer negative controls serially.
Original captures and producer files are read-only. This is a development tool.
"""
import argparse
import hashlib
from pathlib import Path
import struct
from validate_compatibility_gate import check_case, digest, load, require
from validate_corpus import original_rgba, run, write


GREEN = bytes([0, 255, 0, 255]) * 64
RED = bytes([255, 0, 0, 255]) * 64
NATIVE_STEPS = [(60, 3, 2, 3), (2817, 6, 5, 3), (126996, 9, 11, 3)]
LOST_STEPS = [(2, 3, 2, 0), (5, 6, 5, 0), (11, 9, 11, 0)]


def check_frame(directory, frame, mode, kind):
    """Strictly distinguish documented capture loss from native correctness."""
    number = frame['frame']
    lost = kind == 'captured' and not mode & 32
    active = kind == 'captured' and number == 6
    merge_failure = bool(active and mode & 16 and not mode & 2)
    expected_failures = dict(finish_state_failures=0,
                             execute_state_failures=3 if active and mode & 8 else 0,
                             merge_state_failures=3 if merge_failure else 0,
                             parent_finish_state_failures=1 if merge_failure and mode & 4 else 0)
    for key, expected in expected_failures.items():
        require(frame[key] == expected, 'Unexpected state observation: ' + key)
    require([s['step'] for s in frame['steps']] == [0, 1, 2], 'Missing readback')
    for step, expected in enumerate(LOST_STEPS if lost else NATIVE_STEPS):
        observation = frame['steps'][step]
        require(observation['actual'] == list(expected) and
                observation['expected'] == list(NATIVE_STEPS[step]) and
                observation['verified'] is (not lost), 'Incorrect workload fidelity classification')
        require((directory / f'frame-{number}-step-{step}.bin').read_bytes() == struct.pack('<4I', *expected),
                'Readback bytes changed')
    require(frame['image_verified'] is (not lost) and
            (directory / f'frame-{number}.rgba').read_bytes() == (RED if lost else GREEN),
            'Incorrect producer image classification')
    require((directory / f'frame-{number}-baseline.bin').read_bytes() == struct.pack('<4I', 101, 103, 107, 109),
            'Sentinel changed')
    return expected_failures


def main():
    parser = argparse.ArgumentParser(__doc__)
    for name in ['exe', 'producer', 'corpus', 'api', 'validation', 'out']:
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    evidence = dict(schema='FloraGPA merged-list acceptance 1', completed=False, passed=False,
                    producer_manifest_sha256=digest(args.producer / 'manifest.json'),
                    corpus_sha256=digest(args.corpus), api_sha256=digest(args.api),
                    validation_sha256=digest(args.validation / 'validation.json'),
                    exe_sha256=digest(args.exe), counts={}, dynamic_buffer_controls=[])
    write(args.out / 'audit.json', evidence)
    producer = load(args.producer / 'manifest.json')
    api = load(args.api)
    corpus = load(args.corpus)
    validation = load(args.validation / 'validation.json')
    require(producer['completed'] and producer['native_baselines_passed'] and
            producer['delivered_captures'] == 64 and not producer['captured_data_checks_passed'],
            'Producer matrix outcome changed')
    require(api['completed'] and api['producer_manifest_sha256'] == evidence['producer_manifest_sha256'],
            'API evidence provenance')
    require(validation['exe_sha256'] == evidence['exe_sha256'] and
            validation['manifest_sha256'] == evidence['corpus_sha256'], 'Validation provenance')
    for name, expected in producer['producer_files'].items():
        require(digest(args.producer / 'producer' / name) == expected, 'Frozen producer changed')
    require(producer['original_dlls']['shimd3d64.dll'] ==
            'cb0b99d113511cbf7ca9f949d97aa02e4a1f8f110b1f3ad3165d8a81035dee31', 'Wrong shim')
    require(len(producer['cases']) == len(corpus['cases']) == len(api['cases']) ==
            len(validation['cases']) == 64, 'Incomplete evidence')
    counts = dict(cases=64, workload_faithful_captures=32, capture_side_mismatches=32,
                  producer_frames=0, producer_storage_checks=0, producer_images=0,
                  sentinel_readbacks=0, captured_execute_state_failures=0,
                  captured_merge_state_failures=0, captured_parent_finish_state_failures=0,
                  independent_replays=0, original_replays=0, strict_buffer_exports=0,
                  immutable_image_controls=0, dynamic_buffer_controls=0)
    for mode, (item, spec, decoded, case) in enumerate(zip(producer['cases'], corpus['cases'],
                                                         api['cases'], validation['cases'])):
        require(item['mode'] == spec['mode'] == decoded['mode'] == mode and case['id'] == spec['id'],
                'Reordered cases')
        root = args.producer / str(mode)
        require(digest(root / 'capture.gpa_frame') == item['capture_sha256'] == spec['sha256'] ==
                decoded['capture_sha256'], 'Capture identity changed')
        require(not decoded['traditional_lists'] and not decoded['traditional_executes'] and
                len(decoded['dispatches']) == 9, 'Unexpected traditional-list claim')
        expected_maps = [[17, 99, 0, 0]] * 3 if mode & 32 else ([[17, 99, 0, 0]] + [[0, 0, 0, 0]] * 3) * 3
        require([m['words'] for m in decoded['saved_maps']] == expected_maps, 'Saved writes changed')
        for kind in ['native', 'captured']:
            lost = kind == 'captured' and not mode & 32
            process = item['runs'][kind]
            require(process['completed'] and not process['timed_out'] and
                    not process.get('validation_error') and process['exit_code'] == int(lost),
                    'Producer execution outcome changed')
            for name, expected in process['files'].items():
                require(digest(root / kind / name) == expected, 'Producer output changed')
            oracle = load(root / kind / 'oracle.json')
            require(oracle['completed'] and [f['frame'] for f in oracle['frames']] == list(range(12)),
                    'Incomplete producer frames')
            require(oracle['storage_failures'] == (36 if lost else 0) and
                    oracle['image_failures'] == (12 if lost else 0), 'Producer failure totals changed')
            totals = {k: 0 for k in ['finish_state_failures', 'execute_state_failures',
                                    'merge_state_failures', 'parent_finish_state_failures']}
            for frame in oracle['frames']:
                observed = check_frame(root / kind, frame, mode, kind)
                for key, value in observed.items():
                    totals[key] += value
                counts['producer_frames'] += 1
                counts['producer_storage_checks'] += 3
                counts['producer_images'] += 1
                counts['sentinel_readbacks'] += 1
            for key, value in totals.items():
                require(oracle[key] == value, 'State totals changed')
                if kind == 'captured' and key != 'finish_state_failures':
                    counts['captured_' + key] += value
        check_case(case, spec, None, args.validation / case['id'], 2)
        image = GREEN if mode & 32 else RED
        require(spec['reference_rgba_sha256'] == hashlib.sha256(image).hexdigest() and
                spec['native_workload_fidelity'] == ('passed' if mode & 32 else 'capture_side_mismatch'),
                'Conflated replay and workload fidelity')
        require(case['original_status'] == 'repeat_stable' and len(case['original']) == 2,
                'Missing original repetitions')
        for index, original in enumerate(case['original'], 1):
            require(original['exit_code'] == 0 and not original['timed_out'] and original['kernel_completed'] and
                    original['report']['native']['player_sha256'] ==
                    '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b',
                    'Original kernel outcome/identity')
            require(original_rgba(args.validation / case['id'] / f'original-{index}') == image,
                    'Original pixels changed')
            require((args.validation / case['id'] / f'native-{index}/frame.rgba').read_bytes() == image,
                    'Independent pixels changed')
        for boundary in spec['boundaries']:
            for index in [1, 2]:
                output = args.validation / case['id'] / f"boundary-{boundary['resource']}-{boundary['event']}-{index}"
                require(digest(output / 'buffer.bin') == boundary['expected_storage_sha256'], 'Export bytes changed')
        counts['independent_replays'] += 2
        counts['original_replays'] += 2
        counts['strict_buffer_exports'] += 6
        counts['immutable_image_controls'] += len(case.get('controls', []))
    # Dynamic captures already render red: another red image is not a useful
    # negative control. Disable each inner Dispatch and inspect the actual count.
    import os
    env = os.environ.copy()
    env.pop('GPA_LOCAL_INJECT', None)
    for mode in [0, 31]:
        decoded = api['cases'][mode]
        for event in decoded['dispatches']:
            tag = f'dynamic-{mode}-disable-{event}'
            output = args.out / tag
            result = run([args.exe.resolve(), 'buffer', args.producer / str(mode) / 'capture.gpa_frame',
                          '--id', decoded['result_resource'], '--event', decoded['dispatches'][-1],
                          '--disable', event, '--out', output], args.out / (tag + '.log'), 90, env)
            require(result['exit_code'] == 0 and not result['timed_out'], 'Dynamic control execution')
            raw = (output / 'buffer.bin').read_bytes()
            require(raw == struct.pack('<4I', 11, 8, 11, 0), 'Disabled Dispatch did not change execution count')
            result.update(mode=mode, event=event, words=list(struct.unpack('<4I', raw)), storage_sha256=digest(output / 'buffer.bin'))
            evidence['dynamic_buffer_controls'].append(result)
            counts['dynamic_buffer_controls'] += 1
            write(args.out / 'audit.json', evidence)
    evidence.update(completed=True, passed=True, counts=counts)
    write(args.out / 'audit.json', evidence)
    print(counts)


if __name__ == '__main__':
    main()
