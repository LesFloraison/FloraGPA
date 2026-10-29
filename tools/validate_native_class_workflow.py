"""Validate captured class-resource Worker/CLI traces and session exports."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import re
import struct
import zipfile

from validate_native_debug_session import validate as validate_archive, require


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def result(directory, vendor, expected, edited, mode, created):
    accepted = json.loads((directory / 'result.json').read_text())
    require(accepted['ok'] is True, 'Worker result failed')
    context = accepted['context']
    canonical = json.dumps(context, sort_keys=True, ensure_ascii=False, separators=(',', ':')).encode()
    require(hashlib.sha256(canonical).hexdigest() == accepted['context_id'], 'Context hash mismatch')
    require(context['action'] == 'debug-thread' and context['options']['gpa_event'] == 100 and
            context['adapter']['vendor'] == (vendor or 5140) and context['adapter']['warp'] == (vendor == 0),
            'Wrong invocation or adapter')
    require((context['experiment_sha256'] is not None) == edited, 'Wrong experiment context')
    require(set(accepted['artifacts']) == {'trace.jsonl', 'shader.dxbc', 'debug.json'}, 'Unexpected artifacts')
    for name, entry in accepted['artifacts'].items():
        path = directory / name
        require(path.stat().st_size == entry['bytes'] and digest(path) == entry['sha256'], 'Artifact changed')
    records = [json.loads(line) for line in (directory / 'trace.jsonl').read_text().splitlines()]
    header = records[0]
    require(header['stage'] == 'cs' and header['gpa_event'] == 100 and header['gpa_shader'] == 15 and
            header['resource_observation'] == 'before_event_replay' and
            header['input_source'] == 'independent-replay-resource-inputs/12' and
            header['frame_sha256'] == context['frame_sha256'], 'Wrong snapshot provenance')
    require(header['class_bindings'][0]['object'] == 62 and
            header['class_bindings'][0]['this_bits'] == [0, 0, 6 if created else 3, 0], 'Wrong class binding')
    declarations = {r['index']: r['dxbc_opcode'] for r in records if r['record'] == 'instruction'}
    steps = [r for r in records if r['record'] == 'step']
    require(records[-1]['completed'] is True and records[-1]['steps'] == len(steps) and
            120 in [declarations[r['instruction']] for r in steps] and
            121 in [declarations[r['instruction']] for r in steps], 'Missing completed class query execution')
    storage = {}
    validity = {}
    for record in records:
        if record['record'] == 'storage' and record['space'] == 'buffer':
            storage[record['resource']] = bytearray(record['bytes'])
            validity[record['resource']] = bytearray(record['bytes'])
        if record['record'] == 'storage_bytes' and record['space'] == 'buffer':
            start = record['offset']
            data = bytes.fromhex(record['bits_hex'])
            storage[record['resource']][start:start + len(data)] = data
            validity[record['resource']][start:start + len(data)] = bytes.fromhex(record['valid_hex'])
    expected_input = bytearray(struct.pack('<32I', *range(100, 132)))
    if edited:
        offset = (40, 72, 16)[mode]
        struct.pack_into('<I', expected_input, offset, 1100 + offset // 4)
    require(storage[200] == expected_input and validity[200] == bytes([1] * 128), 'Captured input differs')
    require(storage[7] == bytes(16), 'Diagnostic started after the dispatch')
    for index, step in enumerate(steps):
        require(step['sequence'] == index, 'Noncontiguous trace')
        for change in step['buffer_changes']:
            require(change['resource'] == 7, 'Native execution modified the input resource')
            start = change['offset']
            after, before = bytes.fromhex(change['after_hex']), bytes.fromhex(change['before_hex'])
            require(start + len(after) <= 16 and len(before) == len(after) and
                    storage[7][start:start + len(before)] == before, 'Invalid buffer journal')
            require(validity[7][start:start + len(before)] == bytes.fromhex(change['before_valid_hex']),
                    'Invalid buffer validity journal')
            storage[7][start:start + len(after)] = after
            validity[7][start:start + len(after)] = bytes.fromhex(change['after_valid_hex'])
    require(storage[7] == struct.pack('<4I', expected, 0, 0, 0) and validity[7] == bytes([1] * 16),
            'Trace output differs from authored view and buffer contents')
    return accepted


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--log', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--pixel-log', required=True, type=Path)
    parser.add_argument('--pixel-evidence', required=True, type=Path)
    parser.add_argument('--worker', type=Path, default=Path('build/vs2022/Release/FloraGPA.Worker.exe'))
    parser.add_argument('--evidence', action='append', default=[], type=Path)
    args = parser.parse_args()
    cases = []
    for vendor, created, mode in itertools.product((0, 4318, 32902), range(2), range(3)):
        directory = args.directory / f'{vendor}-{created}-{mode}'
        report = json.loads((directory / 'workflow.json').read_text())
        expected = (230, 239, 209)[mode]
        require(report['passed'] is True and [report[k] for k in ('vendor', 'created', 'mode')] ==
                [vendor, created, mode] and report['expected'] == expected and
                report['edited_expected'] == expected + 1000, 'Workflow identity or expectation changed')
        base = result(directory / 'base', vendor, expected, False, mode, created)
        edited = result(directory / 'edited', vendor, expected + 1000, True, mode, created)
        cli = result(directory / 'cli', vendor, expected + 1000, True, mode, created)
        require(all(r['context']['backend_sha256'] == digest(args.worker) for r in (base, edited, cli)),
                'Worker executable differs from the recorded backend')
        require(base['context_id'] != edited['context_id'] and report['base_context'] == base['context'] and
                report['edited_context'] == edited['context'], 'Missing changed analysis context')
        require(digest(directory / 'frame.gpa_frame') == base['context']['frame_sha256'], 'Capture changed')
        require(edited['context']['experiment_sha256'] == cli['context']['experiment_sha256'] and
                edited['context']['effective_experiment'] == cli['context']['effective_experiment'],
                'CLI used a different experiment')
        validate_archive(directory / 'session.zip')
        with zipfile.ZipFile(directory / 'session.zip') as archive:
            state = json.loads(archive.read('state.json'))
        require(state['configuration'] == report['configuration'] and state['position'] == report['inside'],
                'Exported session lost its class-body breakpoint or position')
        cases.append(dict(case=directory.name, base=expected, edited=expected + 1000,
                          context=base['context_id'], edited_context=edited['context_id']))
    log = args.log.read_text()
    require('Finished testing' in log and re.search(r'Totals: 20 passed, 0 failed, 0 skipped', log),
            'Incomplete workflow test run')
    pixel_log = args.pixel_log.read_text()
    require('Finished testing' in pixel_log and
            re.search(r'Totals: 30 passed, 0 failed, 0 skipped', pixel_log), 'Incomplete pixel sampling test run')
    sampling = [json.loads(line[line.index('{'):]) for line in pixel_log.splitlines()
                if '"indexed_sampling":true' in line]
    require(len(sampling) == 9 and {(r['vendor'], r['opcode']) for r in sampling} ==
            set(itertools.product((0, 4318, 32902), (69, 70, 74))), 'Missing indexed sampling row')
    for row in sampling:
        original = json.loads((args.pixel_evidence / str(row['vendor']) / 'results.json').read_text())
        require(original['unexpected_validation_errors'] == 0 and
                original['adapter_vendor'] == (row['vendor'] or 5140) and
                row['helper']['vendor_id'] == original['adapter_vendor'] and
                row['helper']['device_id'] == original['adapter_device'], 'Pixel adapter or validation mismatch')
        require(row['texture_slot'] == 3 and row['sampler_slot'] == 5 and row['coverage'] == [15, 1],
                'Missing relocation or helper coverage')
        require(row['reference'] in original['cases'] and row['reference']['status'] == 'observed' and
                row['reference']['configuration'] == row['reference']['dataset'] == 0 and
                row['reference']['operation'] == {69: 'sample', 70: 'sample_c', 74: 'sample_b'}[row['opcode']],
                'Original sampling reference changed')
    files = sorted(p for p in args.directory.rglob('*') if p.is_file()) + [args.log, Path(__file__),
            Path('tools/validate_native_debug_session.py'), Path('tests/NativeClassResourceTests.cpp')] + args.evidence
    files += [args.worker, args.pixel_log, Path('tests/DxbcPixelSamplingTests.cpp'), Path('src/application/DxbcExecution.cpp'),
              Path('CMakeLists.txt'), Path('tools/native/pixel_sampling_probe.cpp')]
    files += sorted(p for p in args.pixel_evidence.rglob('*') if p.is_file())
    ledger = dict(schema=1, scope='captured_class_buffer_workflow', passed=True, cases=cases,
                  indexed_pixel_sampling=sampling,
                  original_class_dispatch_parity=False, source_debug_parity=False,
                  files=[dict(path=(p.resolve().relative_to(Path.cwd()) if p.resolve().is_relative_to(Path.cwd())
                                    else p.resolve()).as_posix(), bytes=p.stat().st_size,
                              sha256=digest(p)) for p in files])
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(ledger, stream, indent=2)
        stream.write('\n')
    print(json.dumps(dict(passed=True, workflows=len(cases), traces=len(cases) * 3)))


if __name__ == '__main__':
    main()
