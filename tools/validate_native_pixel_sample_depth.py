"""Check sample-depth observations against independent original GPU draws."""
import argparse
import copy
import hashlib
import json
import struct
from pathlib import Path

from validate_native_pixel_history_merger import validate as join
from validate_native_pixel_merger import require

VENDORS = (0, 4318, 32902)
FORMATS = {20: 8, 40: 4, 45: 4, 55: 2}
MODES = ('flat', 'slope', 'bias', 'clamp', 'color-bias')


def validate(r, vendor, samples, sample, fmt, mode):
    join(r, vendor)
    require(r['mode'] == mode and len(r['records']) == 1 and
            r['selection']['sample'] == sample and
            [r['selection']['x'], r['selection']['y']] == [3, 2], 'Fixture selection')
    row = r['records'][0]
    require(len(row['private_output_mergers']) == 1, 'Single primitive invocation')
    merged = row['private_output_mergers'][0]
    m = merged['merger']
    depth = row['sample_depth_observation']
    require(m['sample_count'] == samples and m['depth_format'] == fmt and
            merged['raster_depth_source'] == 'sample_position_observation', 'Depth format and source')
    size = FORMATS[fmt]
    reference = r['reference_depth_bytes']
    require(len(reference) == size and len(m['depth_after']) == samples * size, 'Depth storage shape')
    meaningful = 5 if fmt == 20 else size
    require(m['depth_after'][sample*size:sample*size+meaningful] == reference[:meaningful],
            'Independent original depth differs')
    selected = r['reference_selected_bytes']
    require(len(selected) == len(r['after']['bytes']) and
            all(not valid or a == b for a, b, valid in
                zip(selected, r['after']['bytes'], r['after']['valid_bytes'])),
            'Independent original selected target differs')
    if mode == 'color-bias':
        require(r['selection']['resource'] == 20 and m['color_format'] == 3, 'Color target')
        expected = [merged['raster_inputs']['depth_bits'], 0, 0, 1]
        require(list(struct.unpack('<4I', bytes(selected))) == expected,
                'Original PS position interpolation changed')
    else:
        require(r['selection']['resource'] == 720 and selected == reference, 'Depth-only target')
    different = depth['depth_bits'] != merged['raster_inputs']['depth_bits']
    require(different == (samples == 4 and mode != 'flat'), 'Sample versus pixel-center interpolation')
    if mode == 'flat':
        require(depth['depth_bits'] == 0x3e800000, 'Independent flat-depth arithmetic')
    elif mode == 'slope' and samples == 1:
        require(depth['depth_bits'] == 0x3e700000, 'Independent center-depth arithmetic')
    return different


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    files = {}
    controls = differences = 0
    for vendor in VENDORS:
        for samples in (1, 4):
            for sample in range(samples):
                for fmt in FORMATS:
                    for mode in MODES:
                        path = args.directory / f'sampleDepthHistory-{vendor}-{samples}-{sample}-{fmt}-{mode}.json'
                        raw = path.read_bytes()
                        report = json.loads(raw)
                        context = vendor, samples, sample, fmt, mode
                        differences += validate(report, *context)
                        files[str(path)] = hashlib.sha256(raw).hexdigest()
                        mutations = [
                            lambda r: r['records'][0]['private_output_mergers'][0].update(raster_depth_source='unused'),
                            lambda r: r['records'][0]['sample_depth_observation'].update(depth_bits=0),
                            lambda r: r['records'][0]['sample_depth_observation'].update(position_bits=[0]*4),
                            lambda r: r['records'][0]['sample_depth_observation'].update(geometry_sha256='0'*64),
                            lambda r: r['records'][0]['sample_depth_observation'].update(original_fragment_eligibility=True),
                            lambda r: r['records'][0]['sample_depth_observation'].update(sample=32),
                            lambda r: r['records'][0]['sample_depth_observation'].update(coverage_mask=0),
                            lambda r: r['records'][0]['sample_depth_observation']['observation']['raster_state']['target'].update(format=0),
                            lambda r: r['records'][0]['sample_depth_observation']['observation'].update(sample_frequency=False),
                            lambda r: r['records'][0]['sample_depth_observation']['observation'].update(layer=32),
                            lambda r: r['records'][0]['sample_depth_observation']['observation']['geometry'].update(primitive_ordinal=999),
                            lambda r: r['reference_depth_bytes'].__setitem__(0, r['reference_depth_bytes'][0] ^ 1),
                            lambda r: r['reference_selected_bytes'].__setitem__(0, r['reference_selected_bytes'][0] ^ 1),
                            lambda r: r['records'][0]['private_output_mergers'][0].update(observed_value_matches_checkpoint=False),
                        ]
                        for mutate in mutations:
                            broken = copy.deepcopy(report)
                            mutate(broken)
                            try:
                                validate(broken, *context)
                            except ValueError:
                                controls += 1
                            else:
                                raise ValueError('Accepted corrupted sample-depth report: ' + str(path))
    require(len(files) == 300 and differences == 192, 'Complete sample-depth matrix')
    result = dict(schema=1, completed=True, cases=len(files), sample_center_differences=differences,
                  negative_controls=controls, complete_pixel_history=False, files=files)
    with args.out.open('x', encoding='utf-8') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'files'}))


if __name__ == '__main__':
    main()
