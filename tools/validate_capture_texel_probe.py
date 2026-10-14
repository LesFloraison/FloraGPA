"""Serial texel-probe checks using the independent native fixture's CPU oracle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
from PIL import Image

from validate_capture_msaa_injected import pixels, records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('probe', 'sessions', 'controls', 'qt-bin', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--gpu-vendor', type=int, required=True)
    args = parser.parse_args()
    if not 0 < args.gpu_vendor <= 65535:
        parser.error('Expected nonzero uint16 GPU vendor')
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env['PATH']
    result = dict(completed=False, acceptance_established=False, cases=[], refusals=[])

    def check_png(destination, expected):
        with Image.open(destination / 'frame.png') as image:
            assert image.convert('RGBA').tobytes() == expected

    def save():
        (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')

    def invoke(label, frame, resource, sample, layer, x, y, mode, first, last, budget, success=True,
               destination=None, vendor=None):
        destination = destination or output / label
        command = [str(args.probe.resolve()), str(frame), str(destination),
                   str(args.gpu_vendor if vendor is None else vendor),
                   *map(str, (resource, sample, layer, x, y, mode, first, last, budget))]
        with (output / (label + '.log')).open('wb') as log:
            process = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        assert (process.returncode == 0) == success, label
        return destination, command

    sessions = json.loads(args.sessions.read_text('utf-8-sig'))
    assert len(sessions) == 2 and {s['mode'] for s in sessions} == {24, 25}
    first_case = None
    for session in sessions:
        samples = {24: 2, 25: 4}[session['mode']]
        item = session['frames'][0]
        for control in ('complete', 'missing-initialization', 'missing-application'):
            frame = Path(item['path']) if control == 'complete' else args.controls / f"{session['mode']}-0-{control}.gpa_frame"
            raw, _, entries, _, application, _ = records(frame)
            copies = [at for _, at, _, _, category, kind in entries if (category, kind) == (7, 0x3e)]
            assert len(copies) == 1
            resource = struct.unpack_from('<Q', raw, copies[0] + 16)[0]
            draws = [ident for ident, _, _, _, category, kind in entries if category == 7 and 0x35 <= kind <= 0x3d]
            expected = pixels(samples, item['present_ordinal'], control)

            def words(image, layer, sample, x, y):
                start = ((layer * 7 + y) * 13 * samples + sample * 13 + x) * 4
                return list(image[start:start + 4])

            # Controls without application draws have no original-resource RTV writers;
            # prefix reads still verify that deleted commands affect actual GPU storage.
            for layer, sample, x, y in ((0, 0, 0, 0), (1, samples - 1, 12, 6)):
                mode = 'final' if control == 'missing-application' else 'trace'
                label = f"{session['mode']}-{control}-{layer}"
                begin = draws[0]
                destination, command = invoke(label, frame, resource, sample, layer, x, y, mode, begin, draws[-1], 32)
                report = json.loads((destination / 'report.json').read_text())
                assert report['completed'] and not report['acceptance_established'] and not report['reference_pixels_used']
                observed = report['final_texel']
                assert observed['words'] == words(expected, layer, sample, x, y), label
                assert report['output']['sha256'] == hashlib.sha256(expected).hexdigest()
                assert report['output']['png_round_trip_exact']
                check_png(destination, expected)
                if mode == 'trace':
                    assert report['writers'] and len(report['observations']) == 2 * len(report['writers'])
                result['cases'].append(dict(label=label, command=command, words=observed['words'],
                                           frame_sha256=hashlib.sha256(raw).hexdigest()))
                save()
            if control == 'complete':
                assert len(application) == 3
                for layer in (0, 1):
                    for mode in ('before', 'after'):
                        label = f"{session['mode']}-{mode}-{layer}"
                        event = application[layer]
                        destination, command = invoke(label, frame, resource, 0, layer, 0, 0, mode, event, event, 1)
                        report = json.loads((destination / 'report.json').read_text())
                        oracle = pixels(samples, item['present_ordinal'], 'missing-application' if mode == 'before' else 'complete')
                        assert report['completed'] and len(report['observations']) == 1
                        assert report['observations'][0]['words'] == words(oracle, layer, 0, 0, 0)
                        result['cases'].append(dict(label=label, command=command, words=report['observations'][0]['words']))
                        save()
                    for sample in (0, samples - 1):
                        label = f"{session['mode']}-checkpoint-{layer}-{sample}"
                        event = application[layer]
                        destination, command = invoke(label, frame, resource, sample, layer, 0, 0,
                                                      'checkpoint', event, event, 3)
                        report = json.loads((destination / 'report.json').read_text())
                        assert report['completed'] and len(report['observations']) == 2
                        assert report['output']['sha256'] == hashlib.sha256(expected).hexdigest()
                        assert report['output']['png_round_trip_exact']
                        check_png(destination, expected)
                        for after, observation in enumerate(report['observations']):
                            oracle = pixels(samples, item['present_ordinal'], 'complete' if after else 'missing-application')
                            assert observation['after'] is bool(after)
                            assert observation['words'] == words(oracle, layer, sample, 0, 0)
                        assert report['final_texel']['words'] == words(expected, layer, sample, 0, 0)
                        result['cases'].append(dict(label=label, command=command,
                                                   observations=report['observations']))
                        save()
                if first_case is None:
                    first_case = frame, resource, application, destination

    frame, resource, application, existing = first_case
    defaults = dict(frame=frame, resource=resource, sample=0, layer=0, x=0, y=0,
                    mode='after', first=application[0], last=application[0], budget=1)
    refusals = [('sample', dict(sample=2), 'out of bounds'),
                ('layer', dict(layer=2), 'out of bounds'),
                ('x', dict(x=13), 'out of bounds'), ('y', dict(y=7), 'out of bounds'),
                ('negative', dict(x=-1), 'unsigned decimal'),
                ('overflow', dict(resource=2**64), 'stoull'),
                ('missing', dict(resource=2**64-1), 'Missing capture entry'),
                ('mode', dict(mode='invalid'), 'Unknown probe mode'),
                ('zero-budget', dict(budget=0), 'Read budget'),
                ('wide-budget', dict(budget=1025), 'Read budget'),
                ('vendor', dict(vendor=0), 'Invalid vendor'),
                ('range', dict(last=application[1]), 'identical first/last'),
                ('checkpoint-budget', dict(mode='checkpoint'), 'budget >= 3'),
                ('depth-budget', dict(mode='depth', budget=3), 'Depth checkpoint read budget too small'),
                ('fragment-budget', dict(mode='fragment', budget=7), 'Fragment checkpoint read budget too small'),
                ('fragment-half-budget', dict(mode='fragment-half', budget=7), 'Fragment checkpoint read budget too small'),
                ('fragment-count-budget', dict(mode='fragment-count', budget=7), 'Fragment checkpoint read budget too small'),
                ('fragment-binding-budget', dict(mode='fragment-binding', budget=7), 'Fragment checkpoint read budget too small'),
                ('depth-missing-dsv', dict(mode='depth', budget=7), 'requires a bound DSV'),
                ('non-event', dict(first=resource, last=resource), 'event boundaries'),
                ('existing', dict(destination=existing), 'already exists'),
                ('exhausted', dict(mode='trace', last=application[-1]), 'Read budget exhausted')]
    original = (existing / 'report.json').read_bytes()
    for label, changes, reason in refusals:
        destination, command = invoke('refuse-' + label, **(defaults | changes), success=False)
        assert reason in (output / ('refuse-' + label + '.log')).read_text(errors='replace')
        if label == 'exhausted':
            report = json.loads((destination / 'report.json').read_text())
            assert not report['completed'] and len(report['observations']) == 1
        elif label == 'depth-missing-dsv':
            report = json.loads((destination / 'report.json').read_text())
            assert not report['completed'] and not (destination / 'frame.png').exists()
        elif label == 'existing':
            assert (existing / 'report.json').read_bytes() == original
        else:
            assert not destination.exists()
        result['refusals'].append(dict(label=label, command=command, reason=reason))
        save()
    result['completed'] = True
    save()
    print(json.dumps(dict(completed=True, cases=len(result['cases']), refusals=len(result['refusals']))))


if __name__ == '__main__':
    main()
