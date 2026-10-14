"""Serial development-probe validation against the independent native MSAA oracle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import time

from validate_capture_msaa_injected import pixels, records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('probe', 'sessions', 'controls', 'qt-bin', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--gpu-vendor', type=int, required=True)
    args = parser.parse_args()
    if not 0 < args.gpu_vendor <= 65535:
        parser.error('Expected a PCI vendor in 1..65535')
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env['PATH']
    result = dict(completed=False, acceptance_established=False, cases=[], refusals=[])

    def save():
        (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')

    def invoke(label, frame, destination, vendor, ids, expected_success=True):
        command = [str(args.probe.resolve()), str(frame), str(destination), str(vendor), *map(str, ids)]
        with (output / (label + '.log')).open('wb') as log:
            process = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        assert (process.returncode == 0) == expected_success, label
        return command

    sessions = json.loads(args.sessions.read_text('utf-8-sig'))
    assert len(sessions) == 2 and {s['mode'] for s in sessions} == {24, 25}
    first = None
    for session in sessions:
        samples = {24: 2, 25: 4}[session['mode']]
        for index, item in enumerate(session['frames']):
            cases = [('complete', Path(item['path']))]
            if index == 0:
                cases += [(control, args.controls / f"{session['mode']}-0-{control}.gpa_frame")
                          for control in ('missing-initialization', 'missing-application')]
            for control, frame in cases:
                raw, _, entries, _, _, _ = records(frame)
                # Scope: the existing RGBA8 native fixture has exactly one
                # application MSAA resource and one initialization CopyResource.
                copies = [(at, size) for _, at, size, _, category, kind in entries
                          if category == 7 and kind == 0x3e]
                assert len(copies) == 1
                resource = struct.unpack_from('<Q', raw, copies[0][0] + 16)[0]
                descriptor = next((at for ident, at, _, _, category, kind in entries
                                   if ident == resource and category == 5 and kind == 0x85), None)
                assert descriptor is not None
                shape = struct.unpack_from('<11I', raw, descriptor + 16)
                assert shape[:6] == (13, 7, 1, 2, 27, samples)
                label = f"{session['mode']}-{index}-{control}"
                destination = output / label
                command = invoke(label, frame, destination, args.gpu_vendor, [resource])
                report = json.loads((destination / 'report.json').read_text('utf-8'))
                assert report['completed'] and not report['reference_pixels_used']
                assert report['vendor'] == args.gpu_vendor and report['output_unchanged_after_readbacks']
                expected = pixels(samples, item['present_ordinal'], control)
                assert (destination / 'frame.rgba').read_bytes() == expected
                assert len(report['resources']) == 1
                planes = report['resources'][0]['planes']
                assert len(planes) == 2 * samples
                for plane in planes:
                    oracle = bytearray()
                    for y in range(7):
                        offset = ((plane['layer'] * 7 + y) * 13 * samples + plane['sample'] * 13) * 4
                        oracle.extend(expected[offset:offset + 13 * 4])
                    assert plane['row_bytes'] == 52 and plane['bytes'] == len(oracle)
                    assert (destination / plane['file']).read_bytes() == oracle
                    assert plane['sha256'] == hashlib.sha256(oracle).hexdigest()
                result['cases'].append(dict(label=label, command=command, planes=len(planes),
                                            frame_sha256=hashlib.sha256(raw).hexdigest()))
                save()
                if first is None:
                    first = frame, resource, destination, raw, descriptor
    frame, resource, existing, raw, descriptor = first
    refusals = [('zero-id', args.gpu_vendor, ['0'], output / 'zero'),
                ('negative-id', args.gpu_vendor, ['-1'], output / 'negative'),
                ('overflow-id', args.gpu_vendor, ['18446744073709551616'], output / 'overflow'),
                ('duplicate-id', args.gpu_vendor, [resource, resource], output / 'duplicate'),
                ('missing-id', args.gpu_vendor, ['18446744073709551615'], output / 'missing'),
                ('zero-vendor', 0, [resource], output / 'vendor-zero'),
                ('wide-vendor', 65536, [resource], output / 'vendor-wide'),
                ('existing-output', args.gpu_vendor, [resource], existing)]
    previous = (existing / 'report.json').read_bytes()
    reasons = {'zero-id': 'Expected nonzero', 'negative-id': 'Expected unsigned decimal',
               'overflow-id': 'stoull', 'duplicate-id': 'Duplicate resource',
               'missing-id': 'Missing capture entry', 'zero-vendor': 'Expected nonzero',
               'wide-vendor': 'GPU vendor must', 'existing-output': 'already exists',
               'single-sample': '2x/4x MSAA', 'unsupported-family': 'color families only',
               'budget': 'readback budget exceeded'}
    for label, vendor, ids, destination in refusals:
        invoke(label, frame, destination, vendor, ids, False)
        assert reasons[label] in (output / (label + '.log')).read_text('utf-8')
        assert destination == existing or not destination.exists()
        result['refusals'].append(label)
    assert (existing / 'report.json').read_bytes() == previous
    for label, changes in [('single-sample', [(5, 1)]), ('unsupported-family', [(4, 39)]),
                            ('budget', [(0, 16384), (1, 16384)])]:
        altered = bytearray(raw)
        for field, value in changes:
            struct.pack_into('<I', altered, descriptor + 16 + field * 4, value)
        path = output / (label + '.gpa_frame')
        path.write_bytes(altered)
        destination = output / label
        invoke(label, path, destination, args.gpu_vendor, [resource], False)
        assert reasons[label] in (output / (label + '.log')).read_text('utf-8')
        assert not destination.exists()
        result['refusals'].append(label)
    # Interpose a directory at a plane's file path after the probe has created
    # its output directory, before device setup and readback. Do not count an
    # unrelated startup failure as the intended write-failure observation.
    destination = output / 'plane-write-failure'
    command = [str(args.probe.resolve()), str(frame), str(destination), str(args.gpu_vendor), str(resource)]
    with (output / 'plane-write-failure.log').open('wb') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 30
            while not destination.exists():
                assert process.poll() is None and time.monotonic() < deadline, 'Probe did not start write-failure test'
                time.sleep(0.001)
            (destination / f'{resource}-layer-0-sample-0.bin').mkdir()
            code = process.wait(timeout=120)
        finally:
            if process.poll() is None:
                try:
                    process.wait(timeout=120)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    assert code != 0 and (destination / 'frame.rgba').is_file()
    report = json.loads((destination / 'report.json').read_text('utf-8'))
    assert report['completed'] is False and report.get('error') and not report['resources']
    result['refusals'].append('plane-write-failure')
    result['completed'] = True
    save()
    print(json.dumps(dict(completed=True, files=len(result['cases']),
                         planes=sum(c['planes'] for c in result['cases']),
                         refusals=len(result['refusals']))))


if __name__ == '__main__':
    main()
