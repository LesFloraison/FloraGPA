"""Validate depth-only completion evidence without running a GPU or an external backend."""
import argparse
import copy
import hashlib
import json
import re
import struct
from pathlib import Path

from validate_native_pixel_completion import changed_payloads, chunks, instructions, require, shader_bytes


MODES = ('pass', 'depth-fail', 'discard', 'early-discard', 'shader-depth',
         'coverage', 'null', 'stencil-pass', 'stencil-fail', 'early-stencil-discard')
VENDORS = (0, 4318, 32902)
SELECTIONS = ((1, 0), (4, 0), (4, 3))
MARKER = [0x3f800000, 0x40000000, 0x40400000]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def additional_shader(identity, original, prepared):
    require(identity['schema'] == 1 and identity['kind'] == 'native_pixel_additional_target',
            'Additional target identity')
    require(identity['target'] == 0 and identity['requires_return_instrumentation'] is True and
            identity['original_alpha_available'] is False and
            identity['original_fragment_eligibility'] is None, 'Additional target scope')
    require(identity['source_sha256'] == digest(original) and
            identity['shader_sha256'] == digest(prepared), 'Additional target shader hashes')
    a, b = chunks(original), chunks(prepared)
    code = next(tag for tag in a if tag in (b'SHDR', b'SHEX'))
    version, before = instructions(a[code])
    other, after = instructions(b[code])
    declaration = [0x03000065, 0x00102072, 0]
    require(version == other and after.count(declaration) == 1, 'Additional output declaration')
    require([row for row in after if row != declaration] == before,
            'Additional target changed original executable instructions')
    stale = {b'STAT', b'SDBG', b'SPDB', b'ILDB', b'SRCI', b'ILDN'}
    require(set(b) == set(a) - stale, 'Additional target chunk set')
    for tag, data in a.items():
        if tag in stale or tag == code:
            continue
        if tag not in (b'OSGN', b'OSG5', b'OSG1'):
            require(b[tag] == data, 'Additional target changed unrelated chunk')
            continue
        stride = {b'OSGN': 24, b'OSG5': 28, b'OSG1': 32}[tag]
        bias = 0 if tag == b'OSGN' else 4
        count = struct.unpack_from('<I', data)[0]
        expected = bytearray(data[:8] + bytes(stride) + data[8:])
        struct.pack_into('<I', expected, 0, count + 1)
        for i in range(count):
            offset = 8 + (i + 1) * stride + bias
            name = struct.unpack_from('<I', expected, offset)[0]
            struct.pack_into('<I', expected, offset, name + stride)
        struct.pack_into('<5I', expected, 8 + bias, len(expected), 0, 64, 3, 0)
        expected[8 + bias + 20] = 7
        expected.extend(b'SV_Target\0')
        expected.extend(bytes((-len(expected)) % 4))
        require(b[tag] == expected, 'Additional target changed original output signature')


def observation(row, vendor, mode, samples, sample):
    require(row['status'] == 'observed' and row['mode'] == mode and
            row['vendor_request'] == vendor and row['samples'] == samples and row['sample'] == sample,
            'Fixture selection')
    r = row['private_observation']
    require(r['schema'] == 1 and r['backend'] == 'native-depth-completion/1' and
            r['kind'] == 'native_depth_completion_observation' and
            r['scope'] == 'original_depth_draw_with_private_color_transport', 'Observation identity')
    require(r['original_fragment_eligibility'] is None and r['original_gpu_schedule'] is False and
            r['diagnostic_shader_replacements'] is True and r['extra_uav_slots'] == 0,
            'Observation uncertainty')
    require(r['adapter']['vendor'] == (vendor or 5140) and r['adapter']['warp'] == (vendor == 0),
            'Actual adapter')
    require(r['debug_validation'] == 'no_errors' and r['replay_passes'] == 2 and
            r['original_null_pixel_shader'] == (mode == 'null'), 'Draw contract')
    ref = r['reference_draw']
    require(ref['backend'] == 'native-original-draw/1' and
            ref['diagnostic_shader_replacements'] is False and ref['adapter'] == r['adapter'] and
            ref['frame_sha256'] == r['frame_sha256'] and ref['event'] == r['event'] and
            ref['shaders'] == r['shaders'] and ref['debug_validation'] == 'no_errors',
            'Unmodified reference identity')
    require(r['passing_samples'] == ref['passing_samples'] == row['reference_passing_samples'] and
            r['pipeline_statistics'] == ref['pipeline_statistics'] and
            r['count_domain'] == 'unmodified_draw_on_fresh_private_outputs' and
            r['diagnostic_count_domain'] == 'draw_with_added_color_transport' and
            r['diagnostic_counts_match'] == (r['passing_samples'] == r['diagnostic_passing_samples']),
            'Unmodified and diagnostic counter provenance')
    require(row['observed_depth'] == row['reference_depth'] and len(row['observed_depth']) == 4 and
            len(row['initial_depth']) == 4 and row['original_storage_restored'] is True,
            'Depth/stencil bytes and restoration')
    survives = ('discard' not in mode and mode not in ('depth-fail', 'stencil-fail', 'shader-depth')
                and (mode != 'coverage' or sample % 2 == 0))
    t = r['depth_transport']
    require(t['samples'] == samples and t['sample'] == sample and
            t['width'] == t['height'] == t['layers'] == 1 and
            t['x'] == t['y'] == t['layer'] == 0 and t['layer_domain'] == 'bound_dsv_view',
            'Transport selection')
    require(t['format'] == (2 if samples == 1 else 1) and t['view_format'] == 2 and
            t['budgeted_bytes'] == 48 * samples, 'Transport allocation')
    require(t['marker_present'] == survives and t['rgb_bits'] == (MARKER if survives else [0, 0, 0]),
            'Completion marker')
    target = r['completion_target']
    require(target['slot'] == 0 and target['format'] == 2 and target['cleared_to_zero'] is True and
            target['blend_and_logic_disabled'] is True and target['alpha_to_coverage'] is False,
            'Diagnostic output setup')
    if mode == 'null':
        require(r['shaders'][4]['resource'] == 0 and r['shaders'][4]['sha256'] is None, 'Null original PS')
    else:
        require(r['additional_target_shader']['source_sha256'] == r['shaders'][4]['sha256'],
                'Bound original shader')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--test-log', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--retain', nargs='*', type=Path, default=[],
                        help='Hash development/build logs or other supporting files without treating them as passes')
    args = parser.parse_args()
    require(not args.out.exists(), 'Evidence output must be new')
    log = args.test_log.read_text(encoding='utf-8')
    require(re.search(r'Totals: \d+ passed, 0 failed, 0 skipped, 0 blacklisted, \d+ms', log) is not None,
            'Completed successful Qt run')
    require('Finished testing of DxbcPixelCompletionTests' in log and 'FAIL!' not in log, 'Qt completion')
    files, differences, rejected = [], [], 0

    def retain(path):
        files.append({'path': path.as_posix(), 'sha256': digest(path.read_bytes())})

    retain(args.test_log)
    for vendor in VENDORS:
        for mode in MODES:
            for samples, sample in SELECTIONS:
                tag = f'{vendor}-{mode}-{samples}-{sample}'
                require(f'PASS   : DxbcPixelCompletionTests::depthTransport({tag})' in log,
                        'Missing terminal test pass: ' + tag)
                path = args.directory / f'depthTransport-{tag}.json'
                row = json.loads(path.read_text(encoding='utf-8'))
                observation(row, vendor, mode, samples, sample)
                r = row['private_observation']
                blobs = []
                for suffix in ('.original.dxbc', '.prepared.dxbc', '.marked.dxbc'):
                    binary = Path(str(path) + suffix)
                    blobs.append(binary.read_bytes())
                    retain(binary)
                additional_shader(r['additional_target_shader'], blobs[0], blobs[1])
                shader_bytes(r['completion_shader'], blobs[1], blobs[2])
                if not r['diagnostic_counts_match']:
                    differences.append({'case': tag, 'reference': r['passing_samples'],
                                        'diagnostic': r['diagnostic_passing_samples']})
                for key, value in [('original_fragment_eligibility', True), ('passing_samples', -1),
                                   ('count_domain', 'diagnostic'), ('replay_passes', 1)]:
                    bad = copy.deepcopy(row)
                    bad['private_observation'][key] = value
                    try:
                        observation(bad, vendor, mode, samples, sample)
                    except ValueError:
                        rejected += 1
                    else:
                        raise ValueError('Accepted corrupted observation: ' + key)
                for index, identity, verify in (
                        (1, r['additional_target_shader'], additional_shader),
                        (2, r['completion_shader'], shader_bytes)):
                    for change, payload in changed_payloads(blobs[index]):
                        bad = copy.deepcopy(identity)
                        bad['shader_sha256'] = digest(payload)
                        try:
                            verify(bad, blobs[index - 1], payload)
                        except ValueError:
                            rejected += 1
                        else:
                            raise ValueError('Accepted changed shader payload: ' + change)
                retain(path)
        tag = f'{vendor}-original-control-4-0'
        require(f'PASS   : DxbcPixelCompletionTests::depthTransport({tag})' in log, 'Missing control pass')
        path = args.directory / f'depthTransport-{tag}.json'
        row = json.loads(path.read_text(encoding='utf-8'))
        require(row['reference_depth'] == row['observed_depth'] and
                row['control']['diagnostic_shader_replacements'] is False and
                row['control']['adapter']['vendor'] == (vendor or 5140), 'Original control')
        retain(path)
    retained_runs = []
    for path in args.retain:
        retain(path)
        if path.suffix == '.txt':
            text = path.read_text(encoding='utf-8')
            total = re.search(r'Totals: (\d+) passed, (\d+) failed, (\d+) skipped, (\d+) blacklisted, (\d+)ms', text)
            if total:
                retained_runs.append({'path': path.as_posix(), **dict(zip(
                    ('passed', 'failed', 'skipped', 'blacklisted', 'elapsed_ms'), map(int, total.groups())))})
    result = {'schema': 1, 'kind': 'native_depth_completion_evidence', 'diagnostic_cases': 90,
              'original_controls': 3, 'corrupted_observations_rejected': rejected,
              'count_differences': differences, 'original_fragment_eligibility': None,
              'retained_runs': retained_runs, 'files': files}
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps({key: value for key, value in result.items() if key != 'files'}))


if __name__ == '__main__':
    main()
