"""Verify independent original/native LOD observations and exact active output bits."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path
import re
import struct


def require(value, message):
    if not value:
        raise ValueError(message)


def has_lod(path):
    data = path.read_bytes()
    require(data[:4] == b'DXBC', 'Missing original shader container')
    count, = struct.unpack_from('<I', data, 28)
    for offset in struct.unpack_from('<' + 'I' * count, data, 32):
        tag, size = struct.unpack_from('<4sI', data, offset)
        if tag not in (b'SHEX', b'SHDR'):
            continue
        words = struct.unpack_from('<' + 'I' * (size // 4), data, offset + 8)
        require(words[0] == 0x50 and words[1] == len(words), 'Wrong original shader stage or length')
        pc, found = 2, False
        while pc < len(words):
            length = words[pc] >> 24 & 127
            require(length and pc + length <= len(words), 'Invalid original instruction length')
            found |= words[pc] & 2047 == 108
            pc += length
        return found
    return False


def verify(original, native, vendor):
    require(original['adapter_vendor'] == (vendor or 5140) and original['vendor'] == vendor and
            original['unexpected_validation_errors'] == 0 and original['precision_scenario'] == 0,
            'Wrong original adapter or probe status')
    helper = native['native_helper']
    require(helper['vendor_id'] == original['adapter_vendor'] and
            helper['device_id'] == original['adapter_device'] and helper['warp'] == (vendor == 0) and
            helper['backend'] == 'dxbc-gpu/26', 'Wrong native adapter or backend')
    expected_cases = set(itertools.product(range(14), range(8), (15, 3, 5, 1)))
    require(len(original['cases']) == len(native['cases']) == 448 and
            {(r['configuration'], r['dataset'], r['coverage']) for r in original['cases']} == expected_cases,
            'Incomplete LOD matrix')
    compared, negative, clamped = 0, 0, 0
    for before, after in zip(original['cases'], native['cases']):
        require(before['operation'] == 'lod' and before['offset'] == 0 and before['status'] == 'observed' and
                {k: v for k, v in after.items() if k != 'native'} == before, 'Original observation changed')
        require(len(after['native']) == 4, 'Missing lane results')
        active = before['coverage'] & ~before['discarded']
        for lane in range(4):
            observed = after['native'][lane]
            if not (active & (1 << lane)):
                require(observed is None, 'Inactive output reported as an original observation')
                continue
            require(observed['valid'] == 15 and observed['bits'] == before['output'][lane] and
                    observed['bits'][2:] == [0, 0], 'LOD bits or validity differ')
            values = struct.unpack('<4f', struct.pack('<4I', *observed['bits']))
            negative += values[1] < 0
            clamped += values[0] != values[1]
            compared += 4
    require(compared == native['compared_components'] == 3472 and negative and clamped,
            'Missing output, negative LOD or clamp observations')
    return dict(vendor=vendor, cases=448, compared_components=compared,
                negative_unclamped_values=negative, different_clamped_values=clamped)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, action='append', default=[])
    args = parser.parse_args()
    summaries, rejected = [], 0
    for vendor in (0, 4318, 32902):
        directory = args.directory / f'lod-{vendor}'
        original = json.loads((directory / 'results.json').read_text())
        native = json.loads((directory / 'native-results.json').read_text())
        summaries.append(verify(original, native, vendor))
        for name in {row['shader'] for row in original['cases']}:
            require(has_lod(directory / name), 'Original compiler did not emit LOD')
        for mode in range(3):
            changed = copy.deepcopy(native)
            if mode == 0:
                changed['cases'][0]['native'][0]['bits'][0] ^= 1
            elif mode == 1:
                changed['cases'][0]['native'][0]['valid'] = 14
            else:
                changed['native_helper']['warp'] = not changed['native_helper']['warp']
            try:
                verify(original, changed, vendor)
            except ValueError:
                rejected += 1
            else:
                raise ValueError('Accepted corrupted native evidence')
    log = args.log.read_text()
    totals = re.search(r'Totals: (\d+) passed, (\d+) failed, (\d+) skipped', log)
    require(totals and tuple(map(int, totals.groups())) == (186, 0, 0) and 'Finished testing' in log,
            'Incomplete LOD and sampling regression run')
    files = sorted(p for p in args.directory.rglob('*') if p.is_file()) + [args.log, Path(__file__)] + args.evidence
    files += [Path(p) for p in ('src/application/DxbcExecution.cpp', 'src/application/DxbcGpuOperations.cpp',
              'src/application/DxbcTextureInputs.cpp', 'src/application/DxbcTextureInputs.h',
              'tests/DxbcPixelSamplingTests.cpp', 'tools/native/pixel_sampling_probe.cpp')]
    result = dict(schema=1, scope='native_pixel_lod', passed=True, full_debug_parity=False,
                  adapters=summaries, rejected_corruptions=rejected,
                  files=[dict(path=p.resolve().as_posix(), bytes=p.stat().st_size,
                              sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in files])
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'files'}))


if __name__ == '__main__':
    main()
