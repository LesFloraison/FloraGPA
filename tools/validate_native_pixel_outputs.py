"""Validate owned pixel-output GPU evidence or compact real-capture probe results."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct


def require(value, message):
    if not value:
        raise ValueError(message)


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def common(r):
    require((r['schema'], r['kind'], r['backend'], r['scope']) ==
            (1, 'native_pixel_shader_outputs', 'native-pixel-outputs/1',
             'one_private_pixel_quad_shader_outputs'), 'Wrong result format')
    require(r['original_fragment_eligibility'] is None and r['output_merger_executed'] is False and
            r['captured_gpu_schedule'] is False, 'Unproved fragment execution claimed')
    require(r['execution_model'] == 'coherent-pixel-memory/3', 'Wrong execution model')
    require(len(r['lanes']) == 4 and 0 <= r['selected_lane'] < 4, 'Wrong quad selection')
    require(0 <= r['initial_active_mask'] <= 15 and 0 <= r['final_active_mask'] <= 15, 'Wrong mask')
    mask = 0
    for i, lane in enumerate(r['lanes']):
        require(lane['lane'] == i and lane['initially_covered'] == bool(r['initial_active_mask'] & (1 << i)),
                'Wrong initial lane identity')
        active = lane['initially_covered'] and not lane['discarded']
        require(lane['shader_active'] == active, 'Wrong final shader lane state')
        mask |= int(active) << i
        keys = set()
        for output in lane['outputs']:
            key = (output['kind'], output['index'])
            require(key not in keys, 'Duplicate output identity')
            keys.add(key)
            require(0 < output['declared_mask'] <= 15 and
                    output['valid_mask'] & ~output['declared_mask'] == 0, 'Wrong validity')
            require(len(output['raw_bits']) == 4 and all(type(v) is int and 0 <= v <= 0xffffffff
                                                       for v in output['raw_bits']), 'Wrong raw bits')
    require(mask == r['final_active_mask'], 'Wrong final mask')
    require(r['global_steps'] > 0, 'Missing execution')


def matrix_case(r, vendor, mask, mode):
    common(r)
    require(r['adapter']['vendor'] == (vendor or 5140) and r['adapter']['luid'] != 0, 'Wrong adapter')
    identity = r['reference']['device']['adapter']
    require((r['adapter']['vendor'], r['adapter']['device']) ==
            (identity['vendor_id'], identity['device_id']), 'GPU reference adapter differs')
    require((r['event'], r['shader'], r['selected_lane'], r['initial_active_mask']) == (7, 9, 0, mask),
            'Wrong fixture selection')
    require(r['derivative_batches'] == 1, 'Missing quad derivative')
    for i, lane in enumerate(r['lanes']):
        discard = mode == 'discard' and i % 2 == 0
        coverage = int(mode != 'coverage' or i >= 2)
        require(lane['discarded'] == discard, 'Wrong discard')
        color = [bits(i % 2 + .5), bits(i // 2 + .5), bits(.25), bits(1)]
        integer = ([0xffffffff, 0x80000001, bits(.5), 0x7fffffff] if mode == 'signed' else
                   [0x12345678, 0x80000001, bits(.5), 0xffffffff])
        depth = bits(.25 if mode == 'less' else .75)
        depth_kind = dict(greater='depth_greater_equal', less='depth_less_equal').get(mode, 'depth')
        expected = {('color', 0): ('float32', 15, color),
                    ('color', 1): ('int32' if mode == 'signed' else 'uint32', 15, integer),
                    (depth_kind, 0): ('float32', 1, [depth]),
                    ('coverage', 0): ('uint32', 1, [coverage])}
        require(len(lane['outputs']) == len(expected), 'Wrong output count')
        for output in lane['outputs']:
            numeric, valid, raw = expected[(output['kind'], output['index'])]
            require((output['numeric'], output['declared_mask'], output['valid_mask'], output['minimum_precision']) ==
                    (numeric, valid, valid, 0), 'Wrong output metadata')
            require(output['raw_bits'][:len(raw)] == raw, 'Shader output differs from analytical oracle')
        writes = bool(mask & (1 << i)) and not discard and coverage
        require(r['reference']['color'][i] == (color if writes else [0]*4), 'GPU float output differs')
        require(r['reference']['integer'][i] == (integer if writes else [0]*4), 'GPU integer output differs')
        require(r['reference']['depth'][i] == (depth if writes else bits(.5)), 'GPU depth output differs')


def real_case(path):
    r = json.loads((path / 'pixel-shader-outputs.json').read_text())
    p = json.loads((path / 'report.json').read_text())
    common(r)
    require(p['status'] == 'ok' and p['raster_status'] == 'observed', 'Probe did not complete')
    require(r['frame_sha256'] == p['frame_sha256'] and r['event'] == p['event'], 'Wrong frame/event')
    require(r['shader_sha256'] == digest(path / 'original-ps.dxbc'), 'Wrong bytecode')
    require(all(r['adapter'][key] == p['adapter_' + key] for key in ('vendor', 'device', 'luid')),
            'Input and evaluator adapters differ')
    require(p['output_global_steps'] == r['global_steps'], 'Wrong output execution count')
    terminal = path / 'pixel-terminal-registers.json'
    compared = 0
    if terminal.exists():
        require(p['trace_rewind_verified'] and p['trace']['steps'] == r['global_steps'], 'Trace execution differs')
        require(p['trace']['final_active_mask'] == r['final_active_mask'], 'Trace coverage differs')
        require(p['trace_sha256'] == digest(path / 'pixel-trace.jsonl'), 'Trace digest differs')
        registers = {(v['dxbc_operand_kind'], tuple(v['indices'])): v for v in json.loads(terminal.read_text())}
        for out in r['lanes'][r['selected_lane']]['outputs']:
            kind = dict(color=2, depth=12, coverage=15, depth_greater_equal=38, depth_less_equal=39)[out['kind']]
            reg = registers.get((kind, (out['index'],) if kind == 2 else ()))
            valid = reg['valid_components'] if reg else 0
            require(out['valid_mask'] == valid & out['declared_mask'], 'Journal output validity differs')
            for component in range(4):
                if out['valid_mask'] & (1 << component):
                    require(reg['bits'][component] == out['raw_bits'][component], 'Journal output bits differ')
                    compared += 1
    return dict(name=path.name, event=r['event'], adapter=r['adapter'], global_steps=r['global_steps'],
                private_storage_bytes=r['private_storage_bytes'], output_evaluation_ms=p['output_evaluation_ms'],
                elapsed_ms=p['elapsed_ms'], journal_components_compared=compared,
                output_sha256=digest(path / 'pixel-shader-outputs.json'))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    p.add_argument('--real', action='store_true')
    p.add_argument('--out', required=True, type=Path)
    args = p.parse_args()
    report = dict(schema=1, scope='private_pixel_shader_output_validation', completed=False,
                  cases=[], negative_controls=0)
    if args.real:
        for path in sorted(args.directory.iterdir()):
            if path.is_dir():
                report['cases'].append(real_case(path))
    else:
        expected_names = set()
        for vendor in (0, 4318, 32902):
            for mask in (0, 1, 3, 5, 15):
                for mode in ('ordinary', 'discard', 'coverage', 'greater', 'less', 'signed'):
                    name = f'{vendor}-{mask}-{mode}'
                    expected_names.add(name + '.json')
                    path = args.directory / (name + '.json')
                    r = json.loads(path.read_text())
                    matrix_case(r, vendor, mask, mode)
                    require(r['shader_sha256'] == digest(args.directory / (name + '.dxbc')), 'Wrong fixture bytecode')
                    report['cases'].append(dict(name=name, output_sha256=digest(path), shader_sha256=r['shader_sha256']))
                    for key, value in [('scope', 'original_fragment'), ('original_fragment_eligibility', True),
                                       ('output_merger_executed', True), ('captured_gpu_schedule', True),
                                       ('final_active_mask', r['final_active_mask'] ^ 1), ('event', 8)]:
                        bad = copy.deepcopy(r)
                        bad[key] = value
                        try:
                            matrix_case(bad, vendor, mask, mode)
                        except ValueError:
                            report['negative_controls'] += 1
                        else:
                            raise ValueError('Accepted corrupt output: ' + key)
        require({path.name for path in args.directory.glob('*.json')} == expected_names, 'Extra/missing evidence')
    require(report['cases'], 'No output cases')
    report['completed'] = True
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    print(json.dumps(dict(completed=True, cases=len(report['cases']), negative_controls=report['negative_controls'])))


if __name__ == '__main__':
    main()
