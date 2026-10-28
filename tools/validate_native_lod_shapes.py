"""Validate original/native LOD shape evidence without treating inactive lanes as observations."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path
import re

from validate_native_lod import has_lod, require


def verify(original, native, vendor, dimension):
    require(original['dimension'] == dimension and original['vendor'] == vendor and
            original['adapter_vendor'] == (vendor or 5140) and
            original['unexpected_validation_errors'] == 0, 'Wrong original probe identity')
    helper = native['native_helper']
    require(helper['vendor_id'] == original['adapter_vendor'] and
            helper['device_id'] == original['adapter_device'] and helper['warp'] == (vendor == 0) and
            helper['backend'] == 'dxbc-gpu/27', 'Wrong native helper identity')
    expected = set(itertools.product(range(14), range(8), (15, 3, 5, 1)))
    require(len(original['cases']) == len(native['cases']) == 448 and
            {(r['configuration'], r['dataset'], r['coverage']) for r in original['cases']} == expected,
            'Incomplete original matrix')
    components = 0
    linear_mip_cases = 0
    for before, after in zip(original['cases'], native['cases']):
        require(before['operation'] == 'lod' and before['dimension'] == dimension and
                before['offset'] == 0 and before['status'] == 'observed' and
                {k: v for k, v in after.items() if k != 'native'} == before,
                'Original evidence changed')
        expected_first = 6 if dimension == 10 else 1 if dimension in (7, 8) else 0
        expected_layers = 6 if dimension in (6, 10) else 2 if dimension in (7, 8) else 1
        require(before['first_layer'] == expected_first and before['layer_count'] == expected_layers,
                'Wrong view layer range')
        sampler = before['sampler_descriptor']
        linear_mip_cases += sampler is not None and (sampler[0] & 1) != 0
        require(len(after['native']) == 4, 'Missing native lane records')
        active = before['coverage'] & ~before['discarded']
        for lane in range(4):
            value = after['native'][lane]
            if not (active & (1 << lane)):
                require(value is None, 'Inactive output labeled as original observation')
                continue
            require(value['valid'] == 15 and value['bits'] == before['output'][lane] and
                    value['bits'][2:] == [0, 0], 'LOD output or validity differs')
            components += 4
    require(components == native['compared_components'] == 3472, 'Wrong comparison count')
    require(linear_mip_cases == (64 if dimension == 3 else 384), 'Missing linear mip filter coverage')
    return dict(vendor=vendor, dimension=dimension, cases=448, components=components,
                linear_mip_cases=linear_mip_cases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--log', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--evidence', action='append', type=Path, default=[])
    args = parser.parse_args()
    results, corruptions = [], 0
    for vendor, dimension in itertools.product((0, 4318, 32902), (2, 3, 5, 6, 7, 8, 10)):
        directory = args.directory / (f'lod-{vendor}' if dimension == 3 else f'lod-{vendor}-{dimension}')
        original = json.loads((directory / 'results.json').read_text())
        native = json.loads((directory / 'native-results.json').read_text())
        results.append(verify(original, native, vendor, dimension))
        for shader in {row['shader'] for row in original['cases']}:
            require(has_lod(directory / shader), 'No original LOD opcode')
        for mode in range(3):
            changed = copy.deepcopy(native)
            if mode == 0:
                changed['cases'][0]['native'][0]['bits'][0] ^= 1
            elif mode == 1:
                changed['cases'][0]['native'][0]['valid'] = 14
            else:
                changed['native_helper']['warp'] = not changed['native_helper']['warp']
            try:
                verify(original, changed, vendor, dimension)
            except ValueError:
                corruptions += 1
            else:
                raise ValueError('Accepted corrupted observation')
    log = args.log.read_text()
    totals = re.search(r'Totals: (\d+) passed, (\d+) failed, (\d+) skipped', log)
    require(totals and tuple(map(int, totals.groups())) == (225, 0, 0) and 'Finished testing' in log,
            'Incomplete regression run')
    require(log.count('PASS   : DxbcPixelSamplingTests::lodCoordinateValidity(') == 21,
            'Missing coordinate validity checks')
    sampling = [json.loads(line[line.index('{'):]) for line in log.splitlines()
                if 'QINFO' in line and '::originalPrograms(' in line]
    require(len(sampling) == 147 and sum(r['cases'] for r in sampling) == 9408 and
            sum(r['components'] for r in sampling) == 71424 and
            sum(r['undefined_rejections'] for r in sampling) == 192,
            'Existing implicit sampling coverage changed')
    files = sorted(p for p in args.directory.rglob('*') if p.is_file()) + [args.log, Path(__file__)] + args.evidence
    files += [Path(p) for p in ('src/application/DxbcGpuOperations.cpp',
              'src/application/DxbcTextureInputs.cpp', 'tests/DxbcPixelSamplingTests.cpp',
              'tools/native/pixel_sampling_probe.cpp', 'tools/validate_native_lod.py')]
    result = dict(schema=1, scope='native_lod_shapes', passed=True, full_debug_parity=False,
                  results=results, rejected_corruptions=corruptions,
                  files=[dict(path=p.resolve().as_posix(), bytes=p.stat().st_size,
                              sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in files])
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'files'}))


if __name__ == '__main__':
    main()
