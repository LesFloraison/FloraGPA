"""Development-only timing-query capture/replay and rendered negative control."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

from validate_capture_query_renaming import entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--qt-bin', type=Path)
    parser.add_argument('--original', type=Path, required=True)
    args = parser.parse_args()
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    binary = args.bin.resolve()
    environment = os.environ.copy()
    if args.qt_bin:
        environment['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + environment['PATH']
    raw = args.original.read_bytes()
    observed = []
    for identity, (offset, size, category, kind) in entries(raw).items():
        if category == 7 and kind in (0x30b4, 0x31b4, 0x331d, 0x33e3, 0x34fb) and size == 41:
            present = raw[offset + 28]
            data_size = struct.unpack_from('<I', raw, offset + 33)[0]
            if present == 1 and data_size in (8, 16):
                observed.append(dict(event=identity, type=kind, payload_bytes=size, data_size=data_size,
                                     stored_word=struct.unpack_from('<I', raw, offset + 29)[0]))
    assert {row['data_size'] for row in observed} == {8, 16}, 'Original timing wire evidence missing'
    results = dict(original=str(args.original), original_sha256=hashlib.sha256(raw).hexdigest(),
                   original_observations=observed, cases=[], passed=False,
                   limitation='Legacy GPA retains only the first result word; original GPU timings are not reconstructed.')

    def run(name, *command):
        result = subprocess.run(list(map(str, command)), env=environment, capture_output=True, timeout=90)
        (root / (name + '.log')).write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f'{name} failed ({result.returncode})')

    try:
        images = []
        for mode in ('timing', 'timing-negative', 'mips', 'mips-negative', 'timing-threaded', 'timing-threaded-negative', 'timing-threaded-fl12', 'timing-threaded-fl12-negative', 'timing-empty', 'timing-empty-negative', 'timing-prefix', 'timing-prefix-negative', 'completion', 'completion-negative'):
            capture = root / (mode + '.gpa_frame')
            run(mode, binary / 'FloraCaptureQueryTests.exe', capture, mode)
            out = root / (mode + '-replay')
            run(mode + '-replay', binary / 'FloraGPA.Cli.exe', 'replay', capture, '--out', out)
            report = json.loads((out / 'report.json').read_text(encoding='utf-8'))
            if 'fl12' in mode:
                assert report['feature_level'] == 0xc100, 'Replay silently lowered captured feature level'
            if any(part in mode for part in ('empty', 'prefix', 'completion')):
                assert report['counts']['ordinary_query_completion_synchronizations'] == 1
            pixels = (out / 'frame.rgba').read_bytes()
            expected = bytes((0, 0, 255, 255) if mode.endswith('-negative') else (0, 255, 0, 255)) * (1 if mode.startswith('mips') else 16)
            assert pixels == expected, f'{mode}: actual GPU image differs'
            images.append(pixels)
            results['cases'].append(dict(mode=mode, capture_sha256=hashlib.sha256(capture.read_bytes()).hexdigest(),
                                         rgba_sha256=hashlib.sha256(pixels).hexdigest(), report=report))
        assert images[0] != images[1], 'Removing the GPU Clear did not change replay output'
        assert images[2] != images[3], 'Removing GenerateMips did not change replay output'
        assert images[4] != images[5], 'Removing the worker-thread Clear did not change replay output'
        assert images[6] != images[7], 'Removing the FL12 worker-thread Clear did not change replay output'
        assert images[8] != images[9], 'Removing the empty-prefix Clear did not change replay output'
        assert images[10] != images[11], 'Removing the pre-completion Clear did not change replay output'
        assert images[12] != images[13], 'Removing the EVENT copy completion Clear did not change replay output'
        fixtures = root / 'd24-fixtures'
        run('d24-fixtures', binary / 'FloraCaptureMsaaD24RecordingTests.exe', fixtures)
        d24_images = {}
        for capture in sorted(fixtures.glob('*.gpa_frame')):
            mode = 'd24-' + capture.stem
            out = root / (mode + '-replay')
            run(mode, binary / 'FloraGPA.Cli.exe', 'replay', capture, '--out', out)
            pixels = (out / 'frame.rgba').read_bytes()
            assert pixels == capture.with_suffix('.expected.rgba').read_bytes(), mode
            d24_images[capture.stem] = pixels
            results['cases'].append(dict(mode=mode, rgba_sha256=hashlib.sha256(pixels).hexdigest(),
                                         report=json.loads((out / 'report.json').read_text(encoding='utf-8'))))
        assert len(d24_images) == 4
        for samples in (2, 4):
            assert d24_images[f'{samples}x-complete'] != d24_images[f'{samples}x-negative']
        results['passed'] = True
    finally:
        (root / 'validation.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print(json.dumps(dict(passed=True, output=str(root))))


if __name__ == '__main__':
    main()
