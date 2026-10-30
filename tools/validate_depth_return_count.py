"""Verify depth-only return accumulation and independent original depth storage."""
import argparse
import copy
import itertools
import json
import math
from pathlib import Path
import struct
from validate_pixel_return_count import require, read, sha


def verify(report, vendor, expected=None):
    if report['backend'] == 'native-depth-return-count/2':
        from validate_pixel_return_count import verify as color_count
        color_count(report, vendor, expected, component_type=report['completion_shader']['component_type'], depth_source=True, allow_unknown=True)
        depth = report['depth_transport']; color = report['count_transport']
        require(depth['route'] == 'bound_color_output' and depth['layer_domain'] == 'bound_dsv_view'
                and depth['source_color_slot'] == color['source_slot']
                and report['original_null_pixel_shader'] is False and report['shaders'][4]['resource'] != 0, 'Mixed route')
        for key in ('width','height','layers','samples','sample_quality','x','y','layer','sample'):
            require(depth[key] == color[key], 'Mixed view join: '+key)
        return
    require(report['schema'] == 1 and report['kind'] == 'native_depth_return_count_observation'
            and report['backend'] == 'native-depth-return-count/1'
            and report['scope'] == 'returning_samples_on_private_additive_target', 'Depth count scope')
    require(report['adapter']['vendor'] == (vendor or 5140) and report['adapter']['warp'] == (vendor == 0), 'Adapter')
    require(report['debug_validation'] == 'no_errors' and report['extra_uav_slots'] == 0
            and report['original_fragment_eligibility'] is None and report['original_gpu_schedule'] is False,
            'Diagnostic provenance')
    require(report['diagnostic_working_payload_bytes'] <= report['payload_limit'], 'Budget')
    require(report['original_null_pixel_shader'] == (report['shaders'][4]['resource'] == 0), 'Null PS provenance')
    shader = report['completion_shader']
    added = report['additional_target_shader']
    require(shader['backend'] == 'native-pixel-return-count-shader/1' and shader['component_type'] == 3
            and shader['target'] == 0 and shader['marker_bits'] == [0x3f800000] * 3
            and shader['marker_precision'] == 'full' and shader['extra_uav_slots'] == 0
            and shader['source_sha256'] == added['shader_sha256'], 'Prepared count shader')
    target = report['completion_target']
    require(target['slot'] == 0 and target['format'] == 2 and target['rgb_reduction'] == 'add'
            and target['alpha_reduction'] == 'replace' and target['cleared_to_zero'] is True
            and target['write_mask'] == 15 and target['logic_disabled'] is True
            and target['alpha_to_coverage'] is False and target['blend_and_logic_disabled'] is False,
            'Depth count blend')
    transport = report['depth_transport']
    require(transport['layer_domain'] == 'bound_dsv_view' and transport['view_format'] == 2
            and 0 <= transport['sample'] < transport['samples'] and 0 <= transport['layer'] < transport['layers']
            and 0 <= transport['x'] < transport['width'] and 0 <= transport['y'] < transport['height'], 'Depth selection')
    decoded = report['return_count']; bits = decoded['raw_bits']
    require(len(bits) == 4 and all(type(v) is int and 0 <= v <= 0xffffffff for v in bits), 'Raw count words')
    value = struct.unpack('<f', struct.pack('<I', bits[0]))[0]
    integral = math.isfinite(value) and value >= 0 and math.floor(value) == value
    valid = bits[:3] == [bits[0]] * 3 and integral and value < 2**24
    reason = ('exact_float32_integer' if valid else 'rgb_disagreement' if bits[:3] != [bits[0]] * 3
              else 'invalid_accumulation' if not integral else 'float32_integer_limit')
    require(decoded['valid'] is valid and decoded['reason'] == reason and decoded['exclusive_integer_limit'] == 2**24
            and decoded['original_fragment_eligibility'] is None
            and decoded['count'] == (int(value) if valid else None), 'Count decode')
    if valid:
        require(type(decoded['count']) is int, 'Integer count type')
    if expected is not None:
        require(valid and decoded['count'] == expected, 'Independent count')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    modes = ('pass','depth-fail','discard','early-discard','shader-depth','coverage','null',
             'stencil-pass','stencil-fail','early-stencil-discard','repeat','repeat-stencil','repeat-discard')
    rows = []; rejected = 0
    for vendor, mode, (samples, sample) in itertools.product((0,4318,32902), modes, ((1,0),(4,0),(4,3))):
        data = read(args.root / f'depthTransport-{vendor}-{mode}-{samples}-{sample}.json')
        require([data[k] for k in ('mode','vendor_request','samples','sample')] == [mode,vendor,samples,sample], 'Owned identity')
        expected = (2 if mode == 'repeat-discard' else 3 if mode.startswith('repeat') else
                    int('discard' not in mode and mode not in ('depth-fail','stencil-fail','shader-depth')
                        and (mode != 'coverage' or sample % 2 == 0)))
        require(data['expected_return_count'] == expected and data['observed_depth'] == data['reference_depth']
                and data['original_storage_restored'] is True, 'Original storage reference')
        report = data['return_count_observation']; verify(report,vendor,expected)
        base = args.root / f'depthTransport-{vendor}-{mode}-{samples}-{sample}.json'
        added = report['additional_target_shader']
        require(sha(Path(str(base)+'.original.dxbc')) == added['source_sha256']
                and sha(Path(str(base)+'.prepared.dxbc')) == added['shader_sha256'], 'Saved shader bytes')
        for key in ('frame_sha256','event','adapter','shaders'):
            require(report[key] == data['private_observation'][key], 'Independent draw context')
        if mode == 'repeat-stencil':
            require(data['reference_depth'][3] - data['initial_depth'][3] == expected, 'Original stencil increments')
        require(report['depth_transport']['sample'] == sample and report['depth_transport']['samples'] == samples, 'Sample identity')
        mutations = [lambda r:r['return_count'].update(count=99),lambda r:r['return_count']['raw_bits'].__setitem__(0,0),
            lambda r:r.update(original_fragment_eligibility=True),lambda r:r['completion_target'].update(rgb_reduction='maximum'),
            lambda r:r['depth_transport'].update(layer=99),lambda r:r.update(original_null_pixel_shader=not report['original_null_pixel_shader'])]
        for mutate in mutations:
            bad = copy.deepcopy(report);mutate(bad)
            if bad == report: bad['return_count']['raw_bits'][0] = 0x3f800000
            try: verify(bad,vendor,expected)
            except (ValueError,KeyError): rejected += 1
            else: raise ValueError('Corrupted depth count accepted')
        rows.append({'vendor':vendor,'mode':mode,'samples':samples,'sample':sample,'count':expected})
    result = {'schema':1,'observations':len(rows),'corruptions_rejected':rejected,'cases':rows,
              'full_fragment_history_complete':False}
    args.out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k != 'cases'}))

if __name__ == '__main__': main()
