"""Run native pixel occlusion against the owned twelve-draw GPA fixture.

The fixture and independent reference images are produced once by
run_pixel_history_research.py. This validation never starts original GPA or
RenderDoc; its expected local counts follow the owned draw definitions.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def require(value, message):
    if not value:
        raise ValueError(message)


def validate(r, event, xy, vendor, expected, capture_sha, target, adapter):
    require((r['schema'], r['kind'], r['backend']) ==
            (1, 'native_pixel_occlusion', 'native-pixel-occlusion/1'), 'Unexpected result format')
    require(r['scope'] == 'selected_region_replay' and
            r['isolation'] == 'independent_replay_instance', 'Unsupported scope')
    require((r['event'], r['color_slot'], r['x'], r['y'], r['sample'], r['view']) ==
            (event, 0, xy, xy, 0, target), 'Selection changed')
    require(r['frame_sha256'] == capture_sha, 'Frame changed')
    require(r['adapter']['vendor'] == (vendor or 5140) and r['adapter']['warp'] == (not vendor),
            'Wrong native adapter')
    require(r['adapter']['device'] == adapter['device'], 'Adapter device changed')
    luid = (r['adapter']['luid_high'] & 0xffffffff) << 32 | r['adapter']['luid_low']
    require(luid == adapter['luid'], 'Adapter LUID changed')
    require(r['passing_samples'] == expected, 'Local occlusion count differs')
    require(r['count_semantics'] == 'd3d11_occlusion_query' and
            r['sample_selection_domain'] == 'output_storage' and r['count_domain'] == 'output_samples' and
            not r['forced_sample_mask_implementation_freedom'], 'Count domain changed')
    require(r['original_fragment_eligibility'] is None and r['fragment_records'] is None,
            'Diagnostic promoted to fragment identity')
    require(r['additional_uav_slots'] == 0 and r['replay_count'] == 1 and r['enabled'] and
            r['original_shaders_preserved'] and r['original_depth_stencil_preserved'], 'Pass contract changed')
    require(r['debug_validation'] == 'no_errors', 'D3D validation errors')
    require(r['completion_wait'] in ('fence-event/1', 'bounded-query-poll/1'), 'Unbounded completion')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('fixture_directory', type=Path)
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--qt-bin', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    fixture = args.fixture_directory.resolve()
    frame = fixture / 'fixture.gpa_frame'
    manifest = load(fixture / 'manifest.json')
    commands = load(fixture / 'commands/commands.json')['commands']
    events = [c['id'] for c in commands if c['name'] == 'Draw']
    require(len(events) == 12 and events == manifest['events'], 'Unexpected fixture events')
    capture_sha = digest(frame)
    require(capture_sha == manifest['capture']['sha256'], 'Fixture identity changed')
    report = dict(schema=1, scope='native_selected_region_occlusion_validation', completed=False,
                  capture_sha256=capture_sha, probe_sha256=digest(args.probe), runs=[], negative_controls=[])
    def save():
        (root / 'validation.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    env = dict(os.environ)
    env.pop('GPA_LOCAL_INJECT', None)
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env['PATH']
    for vendor in (0, 4318, 32902):
        reference = load(fixture / f'reference-{vendor}/observations.json')
        require(reference['completed'] and not reference['capture_requested'], 'Invalid reference run')
        for xy in (1, 4):
            for i, event in enumerate(events):
                expected = 0 if i in (3, 4, 5, 6, 7, 10) or i == 2 and xy == 4 else 2 if i == 11 else 1
                label = f'{vendor}-{xy}-{i}'
                output = root / (label + '.json')
                command = list(map(str, [args.probe.resolve(), frame, event, 0, xy, xy, 0, vendor, output]))
                row = dict(vendor=vendor, pixel=[xy, xy], case=i, event=event, expected=expected,
                           command=command, completed=False)
                report['runs'].append(row)
                save()
                with (root / (label + '.log')).open('w', encoding='utf-8') as log:
                    try:
                        process = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                                 timeout=120, creationflags=subprocess.CREATE_NO_WINDOW)
                    except subprocess.TimeoutExpired:
                        row['timed_out'] = True
                        save()
                        raise
                row['exit_code'] = process.returncode
                save()
                require(process.returncode == 0, 'Native probe failed: ' + label)
                result = load(output)
                def check(r):
                    validate(r, event, xy, vendor, expected, capture_sha, manifest['target'], reference['adapter'])
                check(result)
                row.update(completed=True, result_sha256=digest(output), elapsed_ms=result['elapsed_ms'])
                for key, bad in (('passing_samples', expected + 1), ('frame_sha256', '0'*64),
                                 ('event', event + 1), ('original_fragment_eligibility', True),
                                 ('additional_uav_slots', 1), ('debug_validation', 'not_requested'),
                                 ('sample_selection_domain', 'raster_samples'), ('count_domain', 'fragments')):
                    corrupt = copy.deepcopy(result)
                    corrupt[key] = bad
                    try:
                        check(corrupt)
                    except ValueError:
                        report['negative_controls'].append(label + ':' + key)
                    else:
                        raise ValueError('Validator accepted corruption: ' + key)
                save()
    require(digest(frame) == capture_sha, 'Fixture changed during validation')
    report['completed'] = True
    report['draw_count'] = len(report['runs'])
    save()
    print(root / 'validation.json')


if __name__ == '__main__':
    main()
