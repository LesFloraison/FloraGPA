"""Validate native candidate extraction on the owned twelve-draw GPA fixture.

Uses the existing captured fixture and never starts GPA or RenderDoc. Expected
candidate ordinals describe pre-depth geometry, including draws whose original
depth/stencil or pixel shader rejects the fragment.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def require(value, message):
    if not value:
        raise ValueError(message)


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
    manifest = load(fixture / 'manifest.json')
    frame = fixture / 'fixture.gpa_frame'
    capture_sha = digest(frame)
    require(capture_sha == manifest['capture']['sha256'], 'Fixture changed')
    commands = load(fixture / 'commands/commands.json')['commands']
    events = [c['id'] for c in commands if c['name'] == 'Draw']
    require(events == manifest['events'] and len(events) == 12, 'Unexpected fixture event mapping')
    report = dict(schema=1, scope='owned_gpa_candidate_validation', completed=False,
                  capture_sha256=capture_sha, probe_sha256=digest(args.probe), runs=[])
    def save():
        (root / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    env = dict(os.environ)
    env.pop('GPA_LOCAL_INJECT', None)
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env['PATH']
    for vendor in (0, 4318, 32902):
        reference_adapter = load(fixture / f'reference-{vendor}/observations.json')['adapter']
        for xy in (1, 4):
            for case, event in enumerate(events):
                expected = [] if case in (6, 7, 10) or case == 2 and xy == 4 else [0, 1] if case == 11 else [0]
                label = f'{vendor}-{xy}-{case}'
                output = root / label
                command = list(map(str, [args.probe.resolve(), frame, event, xy, xy, 0, 0, vendor, output]))
                row = dict(vendor=vendor, pixel=[xy, xy], case=case, event=event, expected=expected,
                           command=command, completed=False)
                report['runs'].append(row)
                save()
                with (root / (label + '.log')).open('w', encoding='utf-8') as log:
                    try:
                        process = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                                 timeout=150, creationflags=subprocess.CREATE_NO_WINDOW)
                    except subprocess.TimeoutExpired:
                        row['timed_out'] = True
                        save()
                        raise
                row['exit_code'] = process.returncode
                save()
                require(process.returncode == 0, 'Candidate fixture failed: ' + label)
                result = load(output / 'report.json')
                require(result['frame_sha256'] == capture_sha and result['event'] == event, 'Selection changed')
                require(result['scope'] == 'captured_geometry_pre_depth_coverage' and
                        result['original_fragment_eligibility'] is None, 'Scope changed')
                capture = result['capture']
                require(capture['adapter_vendor'] == reference_adapter['vendor'] and
                        capture['adapter_device'] == reference_adapter['device'] and
                        capture['adapter_luid'] == reference_adapter['luid'], 'Adapter changed')
                require(result['unique_primitive_ordinals'] == expected and
                        len(result['observations']) == len(expected), 'Candidate identities differ')
                raw = (output / 'records.bin').read_bytes()
                require(hashlib.sha256(raw).hexdigest() == result['records_sha256'], 'Raw records changed')
                require(digest(output / 'source-geometry.bin') == result['geometry_sha256'], 'Geometry changed')
                require(struct.unpack_from('<I', raw)[0] == len(expected), 'Raw record count differs')
                decoded = []
                for index, observation in enumerate(result['observations']):
                    require(observation['observation_index'] == index, 'Observation index differs')
                    words = struct.unpack_from('<36I', raw, 16 + index * 144)
                    require(words[16:18] == (xy, xy) and words[18] < 2 and words[19] < 2, 'Raw pixel identity differs')
                    lane = words[18] + 2 * words[19]
                    ordinal = words[lane * 4] | words[lane * 4 + 1] << 32
                    require(ordinal == observation['primitive_ordinal'] and words[24 + lane] & 1,
                            'Raw candidate identity/coverage differs')
                    decoded.append(ordinal)
                require(sorted(decoded) == expected, 'Raw candidate set differs')
                row.update(completed=True, result_sha256=digest(output / 'report.json'),
                           elapsed_ms=result['elapsed_ms'])
                save()
    report['completed'] = digest(frame) == capture_sha
    require(report['completed'], 'Fixture changed during validation')
    save()
    print(root / 'manifest.json')


if __name__ == '__main__':
    main()
