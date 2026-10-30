"""Independently verify MSAD bytes, validity, shader identity and reversible journals."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path
import re
import struct
import zipfile

from validate_typed_load_precision import require, shader_rows


MAX = 2**32 - 1
SENTINEL = [19, 31, 47, 59]


def sad(reference, source, accumulator):
    # A wide sum separates defined arithmetic from permitted hardware overflow choices.
    return accumulator + sum(abs(a - b) for a, b in
                             zip(reference.to_bytes(4, 'little'), source.to_bytes(4, 'little')) if a)


def verify(report, native, oracle):
    require(hashlib.sha256(native).hexdigest() == report['native_sha256'], 'Native shader hash')
    stage, rows = shader_rows(native)
    operations = [words for _, words in rows if words[0] & 2047 == 213]
    require(operations, 'Missing actual MSAD instruction')
    require(report['vendor'] in (0, 4318, 32902), 'Adapter selection')
    require(type(report['gpu_supported']) is bool, 'Capability must be explicit')
    if report['kind'] == 'compiled_native_msad':
        require(stage == 1 and report['valid'] == 15, 'Compiled VS validity')
        constants = [0xa100b2c3, 0, 0, 0, 0xd7b0c372, 0x4f57c2a7, 0, 0, 1, 2, 3, 4]
        require(report['constants'] == constants, 'Compiled fixture constants')
        source = constants[4] | (constants[5] << 32)
        expected = [sad(constants[0], (source >> (8 * i)) & MAX, constants[8+i]) for i in range(4)]
        require(report['actual'] == expected, 'Compiled sliding-byte MSAD4')
        require(report['oracle_sha256'] is None, 'Compiled shader is its own GPU input')
        valid = 15
    else:
        require(report['kind'] == 'native_msad', 'MSAD report kind')
        form, pattern = report['form'], report['pattern']
        require(form in range(4) and pattern in range(4), 'Operand selection')
        require(stage == report['stage'] and stage in (0, 1, 5), 'Stage identity')
        reference, source = [0, 0x010000ff, MAX, 0x807f0100], [MAX, 0xff010000, 0, 0x7f8000ff]
        accum = [17, 23, 100, [100, MAX-3, MAX-2, MAX][pattern]]
        validity = [11, 14, 15] if form == 3 else [15]*3
        require(report['reference'] == reference and report['source'] == source and
                report['accum'] == accum and report['input_valid'] == validity, 'Raw fixture inputs')
        mask = 5 if form == 1 else 15
        require(report['mask'] == mask and report['oracle_stage'] == 'vs', 'Transport identity')
        operands = []
        for index, data in enumerate((reference, source, accum)):
            operands.extend([0x4002, *data] if form == 2 else
                            [0x2081b6 if form == 1 else 0x208e46, 0, index])
        instruction = [213, 0x100002 | mask << 4, 0, *operands]
        instruction[0] |= len(instruction) << 24
        require(len(operations) == 1 and list(operations[0]) == instruction, 'Exact MSAD operand encoding')
        expected, valid = SENTINEL.copy(), 15 & ~mask
        for component in range(4):
            if not mask & 1 << component:
                continue
            selected = 3-component if form == 1 else component
            total = sad(reference[selected], source[selected], accum[selected])
            known = (validity[0] & validity[2] & 1 << selected) and (
                not reference[selected] or validity[1] & 1 << selected) and total <= MAX
            expected[component] = total & MAX
            if known:
                valid |= 1 << component
        require(report['valid'] == valid, 'Defined/unknown component mask')
        for component in range(4):
            if valid & 1 << component:
                require(report['actual'][component] == expected[component], 'CPU masked byte sum')
        if report['gpu_supported']:
            require(hashlib.sha256(oracle).hexdigest() == report['oracle_sha256'], 'Oracle shader hash')
            oracle_stage, oracle_rows = shader_rows(oracle)
            require(oracle_stage == 1 and
                    [words for _, words in oracle_rows if words[0] & 2047 == 213] == operations,
                    'GPU must execute the same original operand encoding')
    if report['gpu_supported']:
        require(isinstance(report['gpu'], list) and len(report['gpu']) == 4, 'Missing GPU observation')
        for component in range(4):
            if valid & 1 << component:
                require(report['gpu'][component] == expected[component], 'Independent GPU mismatch')
    else:
        require(report['gpu'] is None, 'Unsupported hardware must not fabricate a GPU observation')
    return valid.bit_count()


def journal(path):
    stage, operations = shader_rows(path.with_suffix('.dxbc').read_bytes())
    words = dict(operations)
    records = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()]
    require(records[0]['shader_sha256'] == hashlib.sha256(path.with_suffix('.dxbc').read_bytes()).hexdigest(),
            'Journal shader identity')
    initial, current, steps = {}, {}, []
    count = 0
    key = lambda lane, reg: (lane, json.dumps(reg, sort_keys=True))
    for record in records:
        if record['record'] == 'register':
            index = key(record['lane'], record['register'])
            initial[index] = current[index] = record['value']
        if record['record'] != 'step':
            continue
        require(record['sequence'] == len(steps), 'Journal continuity')
        steps.append(record)
        for change in record['register_changes']:
            index = key(record['lane'], change['register'])
            if index not in current:
                initial[index] = current[index] = {'bits': [0]*4, 'valid_components': 0, 'mapping_status': [0]*4}
            require(current[index] == change['before'], 'Forward register continuity')
            current[index] = change['after']
        if words[record['word_offset']][0] & 2047 == 213:
            count += 1
            after = record['register_changes'][0]['after']
            overflow = path.stem.endswith('-1')
            require(after['valid_components'] == (7 if overflow else 15), 'Journal overflow validity')
            require(after['bits'][:3] == [17, 532, 1120], 'Journal raw defined bits')
            if not overflow:
                require(after['bits'][3] == 103, 'Journal nonoverflow last component')
    require(count == (4 if stage == 0 else 1), 'Journal MSAD invocation count')
    require(records[-1]['completed'] is True, 'Journal completion')
    for record in reversed(steps):
        for change in reversed(record['register_changes']):
            index = key(record['lane'], change['register'])
            require(current[index] == change['after'], 'Reverse register continuity')
            current[index] = change['before']
    require(current == initial, 'Rewind must restore initial raw values and validity')
    return len(steps)


def workflows(root):
    summaries = []
    for vendor in (0, 4318, 32902):
        directory = root / 'evidence' / f'workflow-{vendor}'
        workflow = json.loads((directory / 'workflow.json').read_text(encoding='utf-8'))
        expected = struct.pack('<4I', 6, 4, 5, 6)
        require(bytes(workflow['original_output']) == expected == bytes(workflow['native_output']),
                'Original CS and native workflow outputs')
        require(workflow['watches'] == [{'expression': 'scores.x', 'status': 'available',
                                       'type': 'uint', 'value': '6'}], 'Source watch value')
        require(workflow['selected'] > 0 and workflow['location'] is not None, 'Mapped source boundary')
        for route in ('worker', 'cli'):
            folder = directory / route
            job = json.loads((folder / 'job.json').read_text(encoding='utf-8'))
            result = json.loads((folder / 'result.json').read_text(encoding='utf-8'))
            require(result['ok'] and result['backend'] == job['backend'] == 'dx11-native/19', 'Worker backend')
            require(result['request_id'] == job['request_id'], 'Request identity')
            context = result['context']
            for key in ('backend', 'backend_sha256', 'action', 'options', 'frame_sha256', 'experiment_sha256'):
                require(context[key] == job[key], 'Worker context ' + key)
            require(context['adapter']['vendor'] == (vendor or 5140), 'Workflow actual adapter')
            require(context['action'] == 'debug-thread' and context['options']['gpa_event'] == 100,
                    'Selected compute invocation')
            require(job['frame_sha256'] == hashlib.sha256((directory / 'frame.gpa_frame').read_bytes()).hexdigest(),
                    'Workflow capture hash')
            require(job['backend_sha256'] == hashlib.sha256((root / 'binaries/FloraGPA.Worker.exe').read_bytes()).hexdigest(),
                    'Executed Worker identity')
            for name, entry in result['artifacts'].items():
                data = (folder / name).read_bytes()
                require(len(data) == entry['bytes'] and hashlib.sha256(data).hexdigest() == entry['sha256'],
                        'Workflow artifact integrity')
            modules = [m.lower() for m in result['execution']['loaded_modules']]
            require(not any(any(term in module for term in
                                ('renderdoc', 'dx11_player.dll', 'dx11_playback.dll', 'intelswtools\\gpa'))
                            for module in modules), 'Unexpected reference backend module')
            records = [json.loads(line) for line in (folder / 'trace.jsonl').read_text(encoding='utf-8').splitlines()]
            initial, validity = bytearray(16), bytearray(16)
            changes = []
            for record in records:
                if record['record'] == 'storage_bytes' and record['space'] == 'buffer' and record['resource'] == 7:
                    offset = record['offset']; data = bytes.fromhex(record['bits_hex'])
                    initial[offset:offset+len(data)] = data
                    validity[offset:offset+len(data)] = bytes.fromhex(record['valid_hex'])
                if record['record'] == 'step':
                    changes.extend(c for c in record['buffer_changes'] if c['resource'] == 7)
            require(initial == struct.pack('<4I', 10, 0, 0, 0), 'Initial workflow storage')
            storage, known = initial.copy(), validity.copy()
            for change in changes:
                offset = change['offset']; before = bytes.fromhex(change['before_hex']); size = len(before)
                require(storage[offset:offset+size] == before and
                        known[offset:offset+size] == bytes.fromhex(change['before_valid_hex']), 'Storage continuity')
                storage[offset:offset+size] = bytes.fromhex(change['after_hex'])
                known[offset:offset+size] = bytes.fromhex(change['after_valid_hex'])
            require(storage == expected and known == bytes([1]*16), 'Terminal workflow storage')
            for change in reversed(changes):
                offset = change['offset']; after = bytes.fromhex(change['after_hex']); size = len(after)
                require(storage[offset:offset+size] == after, 'Reverse storage continuity')
                storage[offset:offset+size] = bytes.fromhex(change['before_hex'])
                known[offset:offset+size] = bytes.fromhex(change['before_valid_hex'])
            require(storage == initial and known == validity, 'Workflow rewind')
            summaries.append({'vendor': vendor, 'route': route, **result['execution']})
        with zipfile.ZipFile(directory / 'session.zip') as archive:
            require(set(archive.namelist()) == {'job.json', 'result.json', 'debug.json', 'shader.dxbc',
                                              'trace.jsonl', 'state.json'}, 'Complete session archive')
            for name in ('job.json', 'result.json', 'debug.json', 'shader.dxbc', 'trace.jsonl'):
                archived = archive.read(name); original = (directory / 'worker' / name).read_bytes()
                require(json.loads(archived) == json.loads(original) if name.endswith('.json') else archived == original,
                        'Archive retained artifact')
    return summaries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--workflow-root', type=Path, help='Separate retained workflow-suite rerun')
    args = parser.parse_args()
    evidence = args.root / 'evidence'
    reports, compared, rejected = [], 0, 0
    for path in sorted(evidence.glob('*.json')):
        report = json.loads(path.read_text(encoding='utf-8'))
        native = path.with_suffix('.native.dxbc').read_bytes()
        oracle_path = path.with_suffix('.oracle.dxbc')
        oracle = oracle_path.read_bytes() if oracle_path.exists() else b''
        compared += verify(report, native, oracle) if report['gpu_supported'] else 0
        reports.append(report)
        mutations = []
        for field, value in [('valid', report['valid'] ^ 1), ('native_sha256', '0'*64), ('vendor', 999)]:
            altered = copy.deepcopy(report); altered[field] = value; mutations.append(altered)
        altered = copy.deepcopy(report)
        component = next(c for c in range(4) if report['valid'] & 1 << c)
        altered['actual'][component] ^= 1
        mutations.append(altered)
        for altered in mutations:
            try:
                verify(altered, native, oracle)
            except ValueError:
                rejected += 1
            else:
                raise ValueError('Corruption accepted')
    raw = [r for r in reports if r['kind'] == 'native_msad']
    expected = set(itertools.product((0, 4318, 32902), (0, 1, 5), range(4), range(4)))
    require(len(raw) == 144 and {(r['vendor'], r['stage'], r['form'], r['pattern']) for r in raw} == expected,
            'Complete raw matrix')
    compiled = [r for r in reports if r['kind'] == 'compiled_native_msad']
    require(len(compiled) == 3 and {r['vendor'] for r in compiled} == {0, 4318, 32902}, 'Compiled matrix')
    traces = sorted(evidence.glob('trace-*.jsonl'))
    require({p.stem for p in traces} == {f'trace-{s}-{o}' for s in (0, 1, 5) for o in (0, 1)}, 'Trace matrix')
    steps = sum(journal(path) for path in traces)
    jobs = workflows(args.workflow_root or args.root)
    log = (args.root / 'FloraDxbcExecutionTests.txt').read_text(encoding='utf-8')
    adapters = []
    for tag, value in re.findall(r'::maskedSad\(([^)]+)\) (\{[^\r\n]+\})', log):
        identity = json.loads(value); requested = int(tag.split('-')[0])
        require(identity['vendor_id'] == (requested or 5140), 'Actual adapter mismatch')
        adapters.append({'selection': tag, **identity})
    require(len(adapters) == 36, 'Actual adapter identity matrix')
    summary = {'schema': 1, 'raw_observations': len(raw), 'compiled_observations': len(compiled),
               'gpu_observations': sum(r['gpu_supported'] for r in reports), 'known_gpu_components': compared,
               'rejected_alterations': rejected, 'journals': len(traces), 'journal_steps': steps,
               'workflow_jobs': jobs,
               'actual_adapters': adapters, 'full_debugger_parity': False}
    args.out.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in summary.items() if k not in ('actual_adapters', 'workflow_jobs')}))


if __name__ == '__main__':
    main()
