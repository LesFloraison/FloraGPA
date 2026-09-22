"""Compare offline scheduled-result acceptance with the unchanged Python reader."""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('reference', 'collections', 'exe', 'qt-bin', 'out'):
        parser.add_argument('--' + key, type=Path, required=True)
    parser.add_argument('--isolated-env', action='store_true')
    args = parser.parse_args(); args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.reference / 'standalone'))
    from md_iteration_results import load_scheduled_result, finite
    from metric_iterations import MetricIterationRunner
    from metric_planner import group_choices
    from metric_values import sample_summary
    def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
    def write(path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, ensure_ascii=False, allow_nan=False), encoding='utf-8')
    def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
    base_folder = args.collections / 'gf2-experiment'
    profile = read(base_folder / 'scheduled-profile.json')
    json_files = {str(p.relative_to(base_folder)).replace('\\', '/'): read(p) for p in base_folder.rglob('*.json')
                  if p.name not in ('scheduled-profile.json', 'report.json')}
    binaries = {row['raw_report']: (base_folder / row['raw_report']).read_bytes() for row in profile['records']}
    original_experiment = (base_folder / 'experiment.json').read_bytes()
    jobs, expected, cases = [], [], []

    def add(folder, p, name, value_error_exact=True):
        before = copy.deepcopy(p)
        try:
            outcome = dict(publisher=load_scheduled_result(folder, p))
        except Exception as exc:
            outcome = dict(error=str(exc), exact=value_error_exact and type(exc) is ValueError, error_type=type(exc).__name__)
        assert before == p, 'Reference reader mutated the profile'
        jobs.append(dict(folder=str(folder.resolve()), profile=p)); expected.append(outcome)
        cases.append(dict(name=name, expected_accept='publisher' in outcome))

    for name in ('gf2-all-ranges', 'gf2-repeat-zero', 'gf2-nonzero', 'gf2-mapped', 'gf2-cached', 'bf1-worker', 'gf2-experiment'):
        folder = args.collections / name
        add(folder, read(folder / 'scheduled-profile.json'), 'saved-' + name)
        assert 'publisher' in expected[-1]

    def synchronize(p, files):
        files['frame-ranges.json'] = copy.deepcopy(p['selection'])
        files['raw-records.json'] = copy.deepcopy(p['records'])
        files['scheduler-audit.json']['protocol'] = copy.deepcopy(p['protocol'])
        for replay in p['replays']:
            a, b = replay['record_span']
            # Invalid spans are a deliberate test input; only sync valid sequence indices.
            if isinstance(a, int) and isinstance(b, int):
                files[f"replay-{replay['replay_index']:03d}/raw-results.json"] = dict(validation=copy.deepcopy(replay), records=copy.deepcopy(p['records'][a:b]))

    def case(name, mutate, sync=False, raw_change=None, experiment_change=False, exact=True, after_write=None):
        folder = args.out / f'{len(cases):03d}-{name}'; folder.mkdir()
        p, files = copy.deepcopy(profile), copy.deepcopy(json_files)
        mutate(p, files)
        if sync: synchronize(p, files)
        for filename, contents in files.items(): write(folder / filename, contents)
        # Preserve the frozen experiment bytes, not a reserialized JSON representation.
        (folder / 'experiment.json').write_bytes(original_experiment + (b' ' if experiment_change else b''))
        for filename, contents in binaries.items():
            path = folder / filename; path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw_change(filename, contents) if raw_change else contents)
        write(folder / 'scheduled-profile.json', p)
        if after_write: after_write(folder)
        add(folder, p, name, exact)

    def put(path, value):
        def mutate(p, files):
            target = p
            for key in path[:-1]: target = target[key]
            target[path[-1]] = value
        return mutate

    for key, value in [('mode', 'uniform'), ('schema_version', 2), ('value_semantics', 'raw'),
                       ('auxiliary_semantics', 'unknown'), ('production_gpa_dependency', True),
                       ('production_gpa_dependency', 0), ('loaded_modules', ['C:/IntelSWTools/GPA/runtime.dll'])]:
        case('header-' + key + '-' + str(value == 0), put([key], value))
    for key in ('schema_version', 'requested_samples', 'warmup_count', 'requested_pass', 'actual_iteration_count'):
        for value in (True, 1., None, '1'):
            case(key + '-' + type(value).__name__, put([key], value))
    case('incomplete', put(['protocol', 'complete'], False))
    for value in ([False, 0, 0], [0., 0, 0], [1, 0, 0], [], [None, 0, 0]):
        case('query-flags-' + str(len(cases)), put(['protocol', 'iterations', 0, 'metrics', 0, 'aux'], value))
    case('descriptor', put(['descriptors', 0, 'name'], 'wrong'))
    case('plan', put(['plan', 'passes', 0, 'set'], 'wrong'))
    case('range-sidecar', lambda p, f: f['frame-ranges.json'].update(extra=True))
    case('raw-sidecar', lambda p, f: f['raw-records.json'][0].update(extra=True))
    case('publisher-count', lambda p, f: f['publisher-values.json']['records'].pop())
    case('publisher-identity', lambda p, f: f['publisher-values.json']['records'][0].update(start_event=-1))
    case('publisher-scalar', lambda p, f: f['publisher-values.json']['records'][0]['values'][0].update(value=123456789.))
    case('publisher-typed', lambda p, f: f['publisher-values.json']['records'][0]['values'][0].update(typed_hex='00'))
    case('empty-range-roster', put(['selection', 'ranges'], []), sync=True)
    case('duplicate-range', lambda p, f: p['selection']['ranges'].append(copy.deepcopy(p['selection']['ranges'][0])), sync=True)
    for key in ('range_index', 'start_event', 'end_event'):
        for value in (True, 1.):
            case('range-' + key + '-' + type(value).__name__, put(['selection', 'ranges', 0, key], value), sync=True)
    for key, value in [('replay_index', 5), ('selected_pass', 5), ('set', 'wrong'), ('metrics', []),
                       ('record_span', [1, 4]), ('record_span', [0., 3.]), ('image_matches', False),
                       ('image_matches', 1), ('range_mapping_valid', False), ('range_mapping_valid', 1),
                       ('rgba_sha256', 'wrong'), ('boundaries', [])]:
        case('replay-' + key + '-' + str(len(cases)), put(['replays', 0, key], value), exact=not key == 'record_span')
    case('raw-result-sidecar', lambda p, f: f['replay-000/raw-results.json'].update(extra=True))
    for key in ('range_index', 'start_event', 'end_event', 'pass_index', 'selected_pass'):
        case('raw-int-' + key, lambda p, f, key=key: p['records'][0].update({key: float(p['records'][0][key])}), sync=True)
    case('raw-range-identity', put(['records', 0, 'range_index'], 99), sync=True)
    def change_path(p, f):
        p['records'][0]['raw_report'] = '../outside.bin'
        f['publisher-values.json']['records'][0]['raw_report'] = '../outside.bin'
    case('raw-canonical-path', change_path, sync=True)
    case('raw-bytes', lambda p, f: None, raw_change=lambda name, data: bytes([data[0] ^ 1]) + data[1:])
    case('protocol-values', put(['protocol', 'values', 0, 0, 'weight'], -42), sync=True)
    case('protocol-range-map', put(['protocol', 'replay_ranges'], []), sync=True)
    case('missing-replay', put(['replays'], []))
    case('unused-replay', lambda p, f: p['replays'].append(copy.deepcopy(p['replays'][0])))
    case('iteration-count', put(['actual_iteration_count'], 99))
    case('matrix-value', put(['metrics', 0, 'median'], -42))
    case('matrix-roster', lambda p, f: p['metrics'].pop())
    case('audit-protocol', lambda p, f: f['scheduler-audit.json']['protocol'].update(extra=True))
    case('audit-failure', lambda p, f: f['scheduler-audit.json'].update(failure={'message': 'failed'}))
    case('audit-trace', lambda p, f: f['scheduler-audit.json']['adapter']['trace'].pop())
    case('arbitration', put(['arbitration'], 'unknown'))
    case('cross-process-adapter', lambda p, f: f['scheduler-audit.json']['adapter'].update(cross_process_priority_mutex=1))
    for key, value in [('cross_process', False), ('resource', 'wrong'), ('closed', False), ('closed', 1),
                       ('depth', 1), ('priority', 7), ('timeouts', 1), ('priorities', []),
                       ('attempts', -1), ('attempts', 0), ('attempts', 1.), ('attempts', True),
                       ('production_gpa_dependency', True), ('production_gpa_dependency', 0)]:
        case('lock-' + key + '-' + str(len(cases)), lambda p, f, k=key, v=value: f['scheduler-audit.json']['adapter']['priority_mutex'].update({k:v}))
    for key in ('owned', 'cached'):
        case('pool-' + key, lambda p, f, k=key: f['scheduler-audit.json']['adapter']['native_pool'].update({k:1}))
    for key, value in [('owned', 1), ('subscriptions', [1]), ('local_lock_depth', 1)]:
        case('adapter-' + key, lambda p, f, k=key, v=value: f['scheduler-audit.json']['adapter'].update({k:v}))
    case('experiment-bytes', lambda p, f: None, experiment_change=True)
    for filename in ('catalog.json', 'frame-ranges.json', 'raw-records.json', 'publisher-values.json', 'replay-000/raw-results.json', 'scheduler-audit.json', 'experiment.json'):
        case('missing-' + filename.replace('/', '-'), lambda p, f: None,
             after_write=lambda folder, name=filename: (folder/name).unlink(), exact=False)
    case('missing-raw-file', lambda p, f: None,
         after_write=lambda folder: (folder/profile['records'][0]['raw_report']).unlink(), exact=False)
    case('invalid-json', lambda p, f: None, after_write=lambda folder: (folder/'catalog.json').write_text('{', encoding='utf-8'), exact=False)

    # Compatibility cases the original reader intentionally accepts.
    case('no-source-frame-needed', put(['frame'], 'does-not-exist.gpa_frame'))
    case('extra-protocol-field', put(['protocol', 'extension'], 'retained'), sync=True)
    case('legacy-no-arbitration', lambda p, f: p.pop('arbitration'))
    case('unchecked-raw-size', put(['records', 0, 'raw_size'], -1), sync=True)
    case('numeric-matrix-alias', put(['metrics', 0, 'measured'], 1))
    case('numeric-lock-alias', lambda p, f: f['scheduler-audit.json']['adapter']['priority_mutex'].update(depth=0., timeouts=False))

    def reconstruct(p, files, unavailable=False, legacy=False):
        if unavailable:
            for row in p['records']: row.update(available=False, unavailable_reasons=['Fixture unavailable'])
            for row in files['publisher-values.json']['records']: row.update(available=False, unavailable_reasons=['Fixture unavailable'])
        if legacy: p.pop('auxiliary_semantics', None)
        descriptions = p['descriptors']; mapped = [(r['start_event'], r['end_event']) for r in p['selection']['ranges']]
        trace = []
        class Transport:
            serial = 0
            def descriptions(self): trace.append(dict(operation='catalog')); return descriptions
            def prepare(self, ids):
                positions = [next(i for i, d in enumerate(descriptions) if d['id'] == mid) for mid in ids]
                self.groups = [[positions[i] for i in g['metrics']] for g in group_choices([descriptions[i]['compatible_sets'] for i in positions])]
                trace.append(dict(operation='prepare', ids=ids, passes=self.groups, flag=False)); return self.groups, False
            def replay(self, index, ranges, flag):
                replay = p['replays'][self.serial]; a, b = replay['record_span']; metrics = []
                for i in self.groups[index]:
                    d = descriptions[i]
                    values = [next(v['value'] for v in row['values'] if v['name']==d['symbol']) if p['records'][j]['available'] else math.nan
                              for j, row in enumerate(files['publisher-values.json']['records'][a:b], a)]
                    metrics.append(dict(metric=d['id'], values=values, aux=[] if legacy else [0]*len(ranges)))
                self.serial += 1; trace.append(dict(operation='replay', pass_index=index, ranges=[list(r) for r in ranges]))
                return dict(metrics=metrics, parallel_ranges=[], flag=False)
        ids = [d['id'] for d in descriptions[:len(p['requested_metrics'])]]
        result = MetricIterationRunner(Transport()).execute(ids, mapped, samples=p['requested_samples'], requested_pass=p['requested_pass'],
            pass_mapping=p['pass_mapping'], weights=p['supplied_weights'])
        p['protocol'].update(finite(result)); p['actual_iteration_count']=result['iteration_count']
        cells=[]
        for info, row in zip(p['selection']['ranges'], result['values'], strict=True):
            for mid, value in zip(ids, row, strict=True):
                d=next(d for d in descriptions if d['id']==mid); definition=d['definition']; samples=finite(value['values'])
                cells.append(dict(range_index=info['range_index'],start_event=info['start_event'],end_event=info['end_event'],metric=d['symbol'],metric_id=mid,
                    label=definition['label'],unit='us' if d['symbol']=='GpuTime' else definition['unit'],measured=bool(samples),values=samples,
                    kind=value['kind'] if samples else d['kind'],weight=finite(value['weight']) if samples else None,**sample_summary(samples)))
        p['metrics']=cells; files['scheduler-audit.json']['adapter']['trace']=trace
    case('unavailable-null-samples', lambda p, f: reconstruct(p, f, unavailable=True), sync=True)
    case('legacy-empty-auxiliary', lambda p, f: reconstruct(p, f, legacy=True), sync=True)
    case('legacy-unavailable', lambda p, f: reconstruct(p, f, unavailable=True, legacy=True), sync=True)
    compatibility = {'no-source-frame-needed', 'extra-protocol-field', 'legacy-no-arbitration', 'unchecked-raw-size',
                     'numeric-matrix-alias', 'numeric-lock-alias', 'unavailable-null-samples', 'legacy-empty-auxiliary', 'legacy-unavailable'}
    assert all(c['expected_accept'] for c in cases if c['name'] in compatibility)

    write(args.out/'request.json', jobs)
    env=os.environ.copy()
    if args.isolated_env:
        env={k:v for k,v in env.items() if k.upper() in {'SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}}
        windows=Path(os.environ['SystemRoot']); env['PATH']=str(windows/'System32')+os.pathsep+str(windows)
    env['PATH']=str(args.qt_bin.resolve())+os.pathsep+env['PATH']
    subprocess.run([str(args.exe.resolve()), '--probe', str((args.out/'request.json').resolve()), str((args.out/'response.json').resolve())], env=env, check=True, timeout=120)
    actual=read(args.out/'response.json'); assert len(actual)==len(expected)
    mismatches=[]
    for i,(a,e,c) in enumerate(zip(actual,expected,cases,strict=True)):
        same=('publisher' in a)==('publisher' in e)
        if 'publisher' in e: same=same and a.get('publisher')==e['publisher']
        elif e['exact']: same=same and a.get('error')==e['error']
        c.update(passed=same, reference_error=e.get('error'), native_error=a.get('error'))
        if not same: mismatches.append(c)
    summary=dict(passed=not mismatches,cases=len(cases),accepted=sum(c['expected_accept'] for c in cases),rejected=sum(not c['expected_accept'] for c in cases),
                 comparisons=cases,mismatches=mismatches,exe_sha256=sha(args.exe),reference_sha256=sha(args.reference/'standalone/md_iteration_results.py'))
    write(args.out/'validation.json',summary)
    print(json.dumps({k:v for k,v in summary.items() if k not in ('comparisons','mismatches')}),flush=True)
    for row in mismatches: print(json.dumps(row),flush=True)
    if mismatches: raise SystemExit(1)


if __name__=='__main__': main()
