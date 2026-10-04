"""Catalog original merged-list captures without equating capture fidelity with replay fidelity.

Development only. API decoding uses the native CLI; the existing Python decoder
independently records saved Map bytes. No capture or reference file is modified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
from validate_compatibility_gate import digest, load, require


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8', newline='\n')


def fields(command):
    return {f['name']: f.get('value') for f in command['fields']}


def main():
    parser = argparse.ArgumentParser(__doc__)
    for name in ['exe', 'producer', 'reference-python', 'out', 'corpus']:
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    require(not args.corpus.exists(), 'Corpus already exists')
    producer = args.producer.resolve(strict=True)
    source = load(producer / 'manifest.json')
    require(source['completed'] and source['native_baselines_passed'] and
            source['delivered_captures'] == 64 and source['modes'] == list(range(64)),
            'Incomplete original matrix')
    sys.path.insert(0, str(args.reference_python.resolve(strict=True)))
    from frame import Frame
    args.out.mkdir(parents=True, exist_ok=False)
    corpus = dict(schema='FloraGPA compatibility corpus 1',
                  profile='GPA 2025 R1 legacy DX11 / original merged lists expanded to immediate commands',
                  acceptance_scope='Replay of saved captures; dynamic-child captures fail uninjected workload fidelity',
                  cases=[])
    evidence = dict(schema='FloraGPA merged-list API evidence 1', completed=False,
                    producer_manifest_sha256=digest(producer / 'manifest.json'),
                    exe_sha256=digest(args.exe), cases=[])
    for item in source['cases']:
        mode = item['mode']
        root = producer / str(mode)
        capture = root / 'capture.gpa_frame'
        require(digest(capture) == item['capture_sha256'], 'Capture changed')
        immutable = bool(mode & 32)
        folder = args.out / str(mode)
        folder.mkdir()
        for command in ['commands', 'command-lists']:
            with (folder / (command + '.log')).open('wb') as log:
                subprocess.run([str(args.exe.resolve()), command, str(capture), '--out', str(folder / command)],
                               check=True, timeout=60, stdout=log, stderr=subprocess.STDOUT)
        commands = load(folder / 'commands/commands.json')['commands']
        lists = load(folder / 'command-lists/command-lists.json')
        require(not lists['command_lists'] and not lists['execute_commands'], 'Traditional list discovered')
        require(all(c['status'] == 'decoded' for c in commands), 'Undecoded API')
        dispatches = [c['id'] for c in commands if c['name'] == 'Dispatch']
        require(len(dispatches) == 9, 'Wrong dispatch count')
        # Read back immediately following each outer execution. The earlier
        # sentinel and later image CopyResource are excluded by this interval.
        copies = [c for c in commands if c['name'] == 'CopyResource' and
                  dispatches[0] < c['id'] < next(c['id'] for c in commands if c['name'] == 'Draw')]
        require(len(copies) == 3, 'Missing outer-execution readback')
        resources = {fields(c)['source'] for c in copies}
        require(len(resources) == 1, 'Inconsistent result resource')
        result = resources.pop()
        boundaries = []
        expected_steps = [(60, 3, 2, 3), (2817, 6, 5, 3), (126996, 9, 11, 3)] if immutable else [
            (2, 3, 2, 0), (5, 6, 5, 0), (11, 9, 11, 0)]
        for step, expected in enumerate(expected_steps):
            raw = (root / f'captured/frame-6-step-{step}.bin').read_bytes()
            require(raw == struct.pack('<4I', *expected), 'Unexpected captured workload result')
            boundaries.append(dict(resource=result, event=dispatches[step * 3 + 2], kind='buffer',
                                   expect_stable=True, expected_storage_sha256=hashlib.sha256(raw).hexdigest()))
        saved_maps = []
        with Frame(str(capture)) as frame:
            for command in commands:
                if command['type'] != 0x246:
                    continue
                values = fields(command)
                updates = frame.updates(values['data'], 16)
                spans = [(int(offset), bytes(raw)) for offset, raw in updates]
                require(len(spans) == 1 and spans[0][0] == 0 and len(spans[0][1]) == 16,
                        'Map was not a complete saved 16-byte write')
                saved_maps.append(dict(event=command['id'], resource=values['resource'],
                                       data=values['data'], words=list(struct.unpack('<4I', spans[0][1]))))
        expected_maps = [[17, 99, 0, 0]] * 3 if immutable else ([[17, 99, 0, 0]] + [[0, 0, 0, 0]] * 3) * 3
        require([m['words'] for m in saved_maps] == expected_maps, 'Saved Map sequence changed')
        raw_image = (root / 'captured/frame-6.rgba').read_bytes()
        require(raw_image == bytes([0, 255, 0, 255] if immutable else [255, 0, 0, 255]) * 64,
                'Unexpected captured image')
        case = dict(id=f'deferred_merge_{mode}', mode=mode,
                    path=f'{producer.name}/{mode}/capture.gpa_frame', sha256=digest(capture),
                    bytes=capture.stat().st_size, family='deferred_merges',
                    origin='self_owned_original_gpa_capture', comparison_policy='exact_golden',
                    reference_rgba_sha256=hashlib.sha256(raw_image).hexdigest(),
                    reference_scope='Captured application frame 6, not uninjected CPU oracle',
                    native_workload_fidelity='passed' if immutable else 'capture_side_mismatch',
                    device_scope='Local producer hardware; original-player adapter/configuration unproven',
                    boundaries=boundaries,
                    controls=[dict(disable_event=event, repeat=1,
                                   expected_rgba_sha256=hashlib.sha256(bytes([255, 0, 0, 255]) * 64).hexdigest())
                              for event in dispatches] if immutable else [])
        corpus['cases'].append(case)
        evidence['cases'].append(dict(mode=mode, capture_sha256=case['sha256'], dispatches=dispatches,
                                     result_resource=result, saved_maps=saved_maps,
                                     traditional_lists=0, traditional_executes=0,
                                     command_count=len(commands)))
        write(args.out / 'api-evidence.json', evidence)
    evidence['completed'] = True
    write(args.out / 'api-evidence.json', evidence)
    write(args.corpus, corpus)
    print('Cataloged 64 captures: 32 workload-faithful, 32 capture-side mismatches; 64 expanded streams.')


if __name__ == '__main__':
    main()
