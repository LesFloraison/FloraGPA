"""Validate native attribute observations, independent GPU controls, and Worker journals."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import struct

BASE = ('linear', 'noperspective', 'centroid', 'noperspective centroid', 'sample',
        'noperspective sample', 'nointerpolation', 'uint', 'sint', 'sparse', 'min16',
        'trigger', 'position', 'position-z', 'sample-id', 'sm4', 'sm41')
EVAL = ('eval-centroid', 'eval-sample', 'eval-snapped', 'eval-snapped-high',
        'eval-centroid-noperspective', 'eval-sample-noperspective', 'eval-snapped-noperspective',
        'eval-centroid-constant', 'eval-sample-constant', 'eval-snapped-constant',
        'eval-centroid-frequency', 'eval-sample-frequency', 'eval-snapped-frequency',
        'eval-centroid-edge', 'eval-sample-edge', 'eval-snapped-edge')


def require(value, message):
    if not value:
        raise ValueError(message)


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def digest(path):
    raw = path.read_bytes()
    return {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}


def modules(paths):
    require(bool(paths), 'Missing loaded modules')
    for path in paths:
        value = path.lower().replace('\\', '/')
        require('renderdoc' not in value and '/intelswtools/gpa/' not in value and
                value.rsplit('/', 1)[-1] not in ('dx11_player.dll', 'dx11_playback.dll'),
                'Forbidden analysis runtime')


def validate_input(directory, replacements=None):
    replacements = replacements or {}
    def document(name):
        return replacements.get(name, read(directory / name))
    report = document('report.json')
    mode, samples, mask = report['mode'], report['samples'], report['mask']
    require(mode in BASE + EVAL and samples in (1, 4), 'Input fixture identity')
    require(report['adapter_vendor'] == (report['vendor_request'] or 5140), 'Input adapter')
    modules(report['modules'])
    def values(name):
        raw = (directory / name).read_bytes()
        require(len(raw) == 256 * samples * 16, 'Input observation size')
        return list(struct.iter_unpack('<4I', raw))
    expected = values('reference.bin')
    oracle = [values(f'oracle-{i}.bin') for i in range(4)]
    observed = [values(f'target-{i}.bin') for i in range(8)]
    comparisons = 0
    for y in range(2, 14):
        for x in range(2, 14):
            for sample in range(samples):
                index = (y * 16 + x) * samples + sample
                location = observed[4][index]
                if mode.endswith('-edge') and location == (16777216,) * 4:
                    require(expected[index] == (16777216,) * 4 and
                            all(v[index] == (16777216,) * 4 for v in oracle + observed), 'Uncovered storage')
                    continue
                px, py, rx, ry = location
                require((px, py) == (x, y) and rx in (0, 1) and ry in (0, 1), 'Pixel quad identity')
                lane = rx + 2 * ry
                for c in range(4):
                    if mask & (1 << c):
                        require(observed[lane][index][c] == expected[index][c], 'Direct GPU observation')
                        for neighbor in range(4):
                            require(observed[neighbor][index][c] == oracle[neighbor][index][c],
                                    'Independent quad oracle')
                            comparisons += 1
    require(comparisons > 0, 'Empty GPU comparison')
    summary = {'case': directory.name, 'compared_quad_components': comparisons,
               'adapter_vendor': report['adapter_vendor'], 'adapter_luid': report['adapter_luid']}
    if mode in EVAL:
        native = document('native-execution.json')
        require(native['original_fragment_eligibility'] is None and not native['output_merger_executed'],
                'Shader outputs must not imply fragment eligibility')
        address = (2 * 16 + 3) * samples + samples - 1
        for lane in range(4):
            output = next(v for v in native['lanes'][lane]['outputs'] if v['kind'] == 'color' and v['index'] == 0)
            require(output['valid_mask'] == 15 and output['raw_bits'] == list(oracle[lane][address]),
                    'C++ shader output differs from independent GPU')
        journal = [json.loads(line) for line in (directory / 'native-trace.jsonl').read_text(encoding='utf-8').splitlines()]
        require(journal[0]['stage'] == 'ps' and journal[-1]['record'] == 'complete' and journal[-1]['completed'],
                'Completed pixel journal')
        if mode.endswith('-constant'):
            require(native['attribute_evaluations'] == [], 'Constant control unexpectedly executed eval_*')
        else:
            records = native['attribute_evaluations']
            require(len(records) == 1 and journal[-1]['attribute_evaluations'] == records, 'Attribute journal provenance')
            record = records[0]
            opcode = 205 if mode.startswith('eval-centroid') else 204 if mode.startswith('eval-sample') else 203
            require(record['backend'] == 'dxbc-pixel-attribute-evaluation/1' and
                    record['request']['opcode'] == opcode and record['adapter_luid'] == report['adapter_luid'],
                    'GPU evaluation identity')
            require(record['request']['location'] == ([0, 0] if opcode == 205 else
                                                     [samples - 1, 0] if opcode == 204 else [8, 7]),
                    'Integer evaluation location')
            undefined = native['undefined_sample']
            require(undefined['backend'] == 'undefined_attribute_sample_index' and undefined['private_draws'] == 0 and
                    all(v['valid_mask'] == 0 and v['bits'] == [0, 0, 0, 0] for v in undefined['quad_values']),
                    'Undefined sample validity')
            require(native['cancelled_cache_hit_rejected'] is True, 'Cancelled cache request')
        if mode.endswith('-edge'):
            require(0 < journal[0]['initial_active_mask'] < 15, 'Partial-coverage helper lanes')
        summary['execution_steps'] = native['global_steps']
        summary['attribute_evaluations'] = len(native['attribute_evaluations'])
    return summary


def validate_job(directory):
    job, result = read(directory / 'job.json'), read(directory / 'result.json')
    require(result['ok'] and result['backend'] == job['backend'] == 'dx11-native/18', 'Worker backend')
    require(result['request_id'] == job['request_id'] and result['context']['action'] == 'debug-pixel', 'Worker identity')
    for key in ('schema', 'backend', 'backend_sha256', 'frame_sha256', 'experiment_sha256', 'action', 'options'):
        require(result['context'][key] == job[key], 'Worker context ' + key)
    require(result['context_id'] == hashlib.sha256(json.dumps(result['context'], sort_keys=True,
            ensure_ascii=False, separators=(',', ':')).encode('utf-8')).hexdigest(), 'Context digest')
    modules(result['execution']['loaded_modules'])
    for name, entry in result['artifacts'].items():
        require(digest(directory / name) == entry, 'Worker artifact digest')
    journal = [json.loads(line) for line in (directory / 'trace.jsonl').read_text(encoding='utf-8').splitlines()]
    require(journal[-1]['completed'] and result['data']['private_history']['observed_storage_restored'], 'Worker completion/restoration')
    require(result['data']['invocation']['original_fragment_eligibility'] is None, 'Private scope')
    mode = directory.name.split('-', 1)[1]
    count = 0
    gpu_bits = None
    if mode.startswith('eval-'):
        count = {'eval-centroid': 1, 'eval-sample': 2, 'eval-snapped': 4}[mode]
        require(len(journal[-1]['attribute_evaluations']) == count, 'Dynamic evaluation locations')
        require(digest(directory / 'frame.gpa_frame')['sha256'] == job['frame_sha256'], 'Synthetic GPA frame digest')
        checkpoint = next(row for row in result['data']['private_history']['records'] if row['primitive_ordinal'] == 1)
        gpu = checkpoint['after']
        require(gpu['exact_storage_bits'] and gpu['valid_bytes'] == [True] * 16, 'Original shader GPU readback')
        gpu_bits = list(struct.unpack('<4I', bytes(gpu['bytes'])))
        final = None
        for row in journal:
            if row['record'] == 'step' and row['lane'] == journal[0]['selected_lane']:
                for change in row['register_changes']:
                    if change['register'] == {'dxbc_operand_kind': 2, 'indices': [0]}:
                        final = change['after']
        require(final and final['valid_components'] == 15 and final['bits'] == gpu_bits,
                'Native output differs from independently executed original shader storage')
    return {'case': directory.name, 'steps': journal[-1]['steps'], 'attribute_evaluations': count,
            'elapsed_ms': result['execution']['elapsed_ms'],
            'peak_working_set_bytes': result['execution']['peak_working_set_bytes'], 'original_gpu_bits': gpu_bits}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--workflow-root', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    workflow = args.workflow_root.resolve() if args.workflow_root else root
    expected = {f'{vendor}-{mode.replace(" ", "-")}-{samples}' for vendor in (0, 4318, 32902)
                for mode in BASE + EVAL for samples in (1, 4)}
    require({p.name for p in (root / 'inputs').iterdir()} == expected, 'Complete GPU input matrix')
    inputs = [validate_input(root / 'inputs' / name) for name in sorted(expected)]
    controls = 0
    for name in sorted(expected):
        directory = root / 'inputs' / name
        report = read(directory / 'report.json')
        if report['mode'] not in EVAL:
            continue
        native = read(directory / 'native-execution.json')
        mutations = [('report.json', lambda r: r.update(adapter_vendor=123)),
                     ('report.json', lambda r: r['modules'].append('C:/unexpected/renderdoc.dll')),
                     ('native-execution.json', lambda r: r.update(original_fragment_eligibility=True)),
                     ('native-execution.json', lambda r: r['lanes'][0]['outputs'][0].update(valid_mask=0))]
        if not report['mode'].endswith('-constant'):
            mutations += [('native-execution.json', lambda r: r.update(cancelled_cache_hit_rejected=False)),
                          ('native-execution.json', lambda r: r['undefined_sample']['quad_values'][0].update(valid_mask=15)),
                          ('native-execution.json', lambda r: r['attribute_evaluations'][0]['request'].update(opcode=0))]
        for file, mutate in mutations:
            changed = copy.deepcopy(report if file == 'report.json' else native)
            mutate(changed)
            try:
                validate_input(directory, {file: changed})
            except (ValueError, KeyError, StopIteration):
                controls += 1
            else:
                raise ValueError('Corrupted input evidence was accepted: ' + name)
    modes = ('constant', 'derivative', 'sample', 'discard', 'source', 'uav', 'eval-centroid', 'eval-sample', 'eval-snapped')
    jobs = [validate_job(workflow / 'jobs' / f'{vendor}-{mode}') for vendor in (0, 4318, 32902) for mode in modes]
    totals = {}
    workflow_suites = ('FloraNativePixelDebugTests', 'FloraNativeDebugSessionTests', 'FloraNativeComputeDebugTests')
    runs = [(root, run) for run in read(root / 'exits.json')
            if root == workflow or run['suite'] not in workflow_suites]
    if root != workflow:
        runs += [(workflow, run) for run in read(workflow / 'exits.json')]
    for folder, run in runs:
        require(run['exit_code'] == 0, 'Failed suite')
        text = (folder / (run['suite'] + '.txt')).read_text(encoding='utf-8')
        match = re.search(r'Totals: (\d+) passed, 0 failed, 0 skipped, 0 blacklisted', text)
        require(match, 'Suite failure or skip')
        totals[run['suite']] = int(match.group(1))
    require(len(totals) == 8, 'Complete serial regression suites')
    manifests = []
    for folder in dict.fromkeys((root, workflow)):
        for name, identity in read(folder / 'binary-identities.json').items():
            require(digest(folder / 'binaries' / name) == identity, 'Retained executable identity')
            if root != workflow and name != 'FloraNativePixelDebugTests.exe':
                require(identity == read(root / 'binary-identities.json')[name], 'Split-run executable changed')
        manifest = folder / 'artifact-identities.json'
        artifacts = {str(p.relative_to(folder)).replace('\\', '/'): digest(p) for p in sorted(folder.rglob('*'))
                     if p.is_file() and p != manifest}
        manifest_text = json.dumps(artifacts, indent=2) + '\n'
        if manifest.exists():
            require(manifest.read_text(encoding='utf-8') == manifest_text, 'Retained artifact manifest changed')
        else:
            manifest.write_text(manifest_text, encoding='utf-8')
        manifests.append({'path': str(manifest), **digest(manifest), 'entries': len(artifacts)})
    report = {'schema': 1, 'kind': 'native_pixel_attribute_evaluation_evidence', 'root': str(root), 'workflow_root': str(workflow),
              'tests': totals, 'total_passed': sum(totals.values()), 'inputs': inputs, 'jobs': jobs,
              'artifact_manifests': manifests,
              'corrupted_inputs_rejected': controls, 'original_fragment_eligibility': None,
              'remaining': ['Minimum-precision eval operands', 'Full P1/P5/P6 parity', 'P7/P8 dependency removal and release']}
    if root != workflow:
        report['superseded_runs'] = [run for run in read(root / 'exits.json') if run['suite'] in workflow_suites]
    args.out.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'tests': report['total_passed'], 'inputs': len(inputs), 'jobs': len(jobs), 'artifacts': sum(m['entries'] for m in manifests)}))


if __name__ == '__main__':
    main()
