"""Verify native double source/watch workflow evidence and exported session recovery."""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import struct
import zipfile

from validate_native_debug_session import validate as validate_archive, require


TOTALS = {0x3ff0000000000001, 0x3ff8000000000001, 0x4000000000000000, 0x4004000000000000}
OUTPUT = list(struct.pack('<4I', 0, 0x40040000, 0, 0xc0040000))


def bits(value):
    return struct.unpack('<Q', struct.pack('<d', value))[0]


def verify(report, state, result, completion):
    vendor = report['vendor']
    require(vendor in (0, 4318), 'Wrong workflow adapter')
    adapter = report['analysis_context']['adapter']
    require(adapter['vendor'] == (vendor or 5140) and adapter['warp'] == (vendor == 0), 'Adapter substitution')
    require(report['analysis_context'] == result['context'], 'Workflow context differs from accepted result')
    gpu = completion['gpu']
    require(gpu['backend'] == 'dxbc-gpu/21' and completion['gpu_operations'] == 21, 'Double helper was not used')
    for source, target in (('vendor_id', 'vendor'), ('device_id', 'device'), ('driver_version', 'driver_version'),
                           ('luid_low', 'luid_low'), ('luid_high', 'luid_high'), ('warp', 'warp')):
        require(gpu[source] == adapter[target], 'Double helper used a different adapter')
    require(report['original_output'] == report['native_output'] == OUTPUT, 'Independent output differs')
    require(set(report['source_values']) == TOTALS, 'Missing precise loop values')
    observed = set()
    for row in report['observations']:
        source = row['source']
        require(source['name'] == 'total' and source['type'] == 'double' and source['status'] == 'available',
                'Wrong observed source variable')
        require(bits(float(row['watch'])) == source['bits'] == bits(float(source['value'])), 'Source/watch precision loss')
        require(source['bits'] in TOTALS and row['location'] is not None, 'Unknown or unmapped loop value')
        observed.add(source['bits'])
    require(observed == TOTALS, 'Incomplete observed source values')
    require(report['conditional_encounter'] == 3 and float(report['conditional_total']) == 2.0,
            'Conditional source breakpoint differs')
    require(any(row['position'] == report['conditional_position'] and row['source']['bits'] == bits(2.0)
                for row in report['observations']), 'Conditional position has a different source value')
    require(state['position'] == report['selected'] and state['registers'] == report['raw_registers'], 'Saved register state differs')
    require(state['configuration'] == report['configuration'] and state['watches'] == report['watch_results'],
            'Saved debug configuration differs')
    require(len(report['watch_results']) == 1 and report['watch_results'][0]['status'] == 'available' and
            bits(float(report['watch_results'][0]['value'])) == 0x3ff0000000000001, 'Saved double watch lost precision')
    require(len(report['raw_registers']) == len(report['double_registers']), 'Missing paired register rows')
    for raw, shown in zip(report['raw_registers'], report['double_registers']):
        require(shown[0] == raw['name'] and shown[2] == 'double', 'Wrong register interpretation')
        values = shown[1].split(', ')
        require(len(values) == 2, 'Double register does not contain two pairs')
        for pair, text in enumerate(values):
            first = 2 * pair
            known = all(raw['written'][first:first + 2])
            if not known:
                require(text == 'unknown', 'Partial pair presented as known')
                continue
            expected = raw['bits'][first] | (raw['bits'][first + 1] << 32)
            value = struct.unpack('<d', struct.pack('<Q', expected))[0]
            if math.isnan(value):
                require(text == 'nan', 'NaN display differs')
            else:
                require(bits(float(text)) == expected, 'Paired register display lost bits')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    reports, controls = [], []
    for vendor in (0, 4318):
        directory = args.directory / str(vendor)
        report = json.loads((directory / 'workflow.json').read_text(encoding='utf-8'))
        result = json.loads((directory / 'worker/result.json').read_text(encoding='utf-8'))
        with zipfile.ZipFile(directory / 'session.zip') as archive:
            state = json.loads(archive.read('state.json'))
        records = [json.loads(line) for line in (directory / 'worker/trace.jsonl').read_text(encoding='utf-8').splitlines()]
        completion = records[-1]
        verify(report, state, result, completion)
        require(report['vendor'] == vendor, 'Wrong directory identity')
        for backend in ('worker', 'cli'):
            accepted = json.loads((directory / backend / 'result.json').read_text(encoding='utf-8'))
            for name, artifact in accepted['artifacts'].items():
                require(name in ('trace.jsonl', 'shader.dxbc', 'debug.json'), 'Unexpected artifact')
                data = (directory / backend / name).read_bytes()
                require(len(data) == artifact['bytes'] and hashlib.sha256(data).hexdigest() == artifact['sha256'],
                        'Worker/CLI artifact digest differs')
            require(accepted['data']['invocation'] == result['data']['invocation'], 'CLI invocation differs')
            for module in accepted['execution']['loaded_modules']:
                lowered = module.lower().replace('\\', '/')
                require('renderdoc' not in lowered and '/intelswtools/gpa/' not in lowered,
                        'Unexpected diagnostic runtime module')
        archive_result = validate_archive(directory / 'session.zip', directory / 'worker', args.probe)
        reports.append(dict(vendor=vendor, observations=len(report['observations']), selected=report['selected'],
                            conditional_position=report['conditional_position'], archive=archive_result))
        mutations = {
            'native_output': lambda r: r['native_output'].__setitem__(0, 1),
            'reference_output': lambda r: r['original_output'].__setitem__(0, 1),
            'watch_bits': lambda r: r['observations'][0].update(watch='1.0'),
            'missing_value': lambda r: r['source_values'].pop(),
            'conditional_value': lambda r: r.update(conditional_total='1.5'),
            'conditional_count': lambda r: r.update(conditional_encounter=2),
            'raw_word': lambda r: r['raw_registers'][0]['bits'].__setitem__(0, 7),
            'paired_display': lambda r: r['double_registers'][0].__setitem__(1, '1.0, 1.0'),
            'adapter': lambda r: r['analysis_context']['adapter'].update(luid_low=0),
        }
        for name, mutate in mutations.items():
            changed = copy.deepcopy(report)
            mutate(changed)
            try:
                verify(changed, state, result, completion)
            except ValueError as error:
                controls.append(dict(vendor=vendor, case=name, reason=str(error)))
            else:
                raise ValueError('Corrupted workflow accepted: ' + name)
    output = dict(schema=1, scope='double_compute_source_workflow', complete_debugger_parity=False,
                  workflows=reports, negative_controls=controls)
    with args.out.open('x', encoding='utf-8') as file:
        json.dump(output, file, indent=2)
        file.write('\n')
    print(json.dumps(dict(workflows=len(reports), archive_reloads=len(reports), negative_controls=len(controls))))


if __name__ == '__main__':
    main()
