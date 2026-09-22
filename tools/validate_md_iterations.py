"""Validate native scheduled collection with the original Python result reader."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from unittest.mock import patch
from PIL import Image


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ('reference', 'exe', 'worker', 'probe-exe', 'qt-bin', 'out'):
        p.add_argument('--' + key, type=Path, required=True)
    p.add_argument('--isolated-env', action='store_true'); args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=False); sys.path.insert(0, str(args.reference / 'standalone'))
    from md_iteration_results import load_scheduled_result
    from md_frame_ranges import select_frame_ranges
    from frame import Frame
    import md_iterations
    env = os.environ.copy()
    if args.isolated_env:
        env = {k: v for k, v in env.items() if k.upper() in {'SYSTEMROOT', 'SYSTEMDRIVE', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA'}}
        windows = Path(os.environ['SystemRoot']); env['PATH'] = str(windows / 'System32') + os.pathsep + str(windows)
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env['PATH']
    checks = []
    def check(name, value):
        checks.append(dict(name=name, passed=bool(value)))
        if not value: raise AssertionError(name)
    def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
    def write(path, value): path.write_text(json.dumps(value), encoding='utf-8')
    def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

    requests = [dict(symbols=['A'])]
    for key in ('samples', 'warmup'):
        for value in (0, 1, 100, -1, 101, True, 1., '3', None): requests.append(dict(symbols=['A'], **{key: value}))
    for weights in ([1, 2.5], [0], [], [True], [None], ['1']): requests.append(dict(symbols=['A'], weights=weights))
    for symbols in ([], ['A', 'A'], ['A', 'B'], ['']): requests.append(dict(symbols=symbols))
    for selected, mapping in ((0, []), (1, [1, 0]), (0xffffffff, []), (-1, []), (True, []), (1, []),
                              (1, [0]), (0, [-1]), (0, [True]), (0, [0xffffffff])):
        requests.append(dict(symbols=['A'], requested_pass=selected, pass_mapping=mapping))
    class AtDevice(Exception): pass
    expected = []
    for i, request in enumerate(requests):
        cfg = dict(symbols=request['symbols'], samples=request.get('samples', 1), warmup=request.get('warmup', 1),
                   requested_pass=request.get('requested_pass', 0xffffffff), pass_mapping=request.get('pass_mapping', []),
                   weights=request.get('weights', []), frame_ranges=request.get('frame_ranges', None))
        try:
            with patch.object(md_iterations, 'create_vendor_device', side_effect=AtDevice):
                md_iterations.collect('not-opened.gpa_frame', args.out / f'request-reference-{i}', cfg['symbols'],
                    **{k: v for k, v in cfg.items() if k != 'symbols'})
        except AtDevice: expected.append(cfg)
        except Exception as exc: expected.append(dict(error=str(exc)))
    request_path, response_path = args.out / 'requests.json', args.out / 'request-results.json'
    write(request_path, requests)
    subprocess.run([str(args.probe_exe.resolve()), '--probe', str(request_path.resolve()), str(response_path.resolve())],
                   env=env, check=True, timeout=30)
    check('request validation matches original collector', read(response_path) == expected)

    gf = args.reference / 'GF2_Exilium_2026_03_03__00_19_35.gpa_frame'
    bf = args.reference / 'bf1_2026_01_21__16_53_05.gpa_frame'
    symbols = ['GpuTime', 'EuActive', 'Sampler00InputAvailable', 'Sampler00OutputReady']
    profiles = []
    def run(label, capture=gf, indices=(4, 0, 2), selected='all', samples=1, mapping=None, weights=None,
            experiment=None, worker=False, handshake=False, warmup=0):
        folder = args.out / label
        cmd = [str((args.worker if worker else args.exe).resolve()), 'metric-iterations', str(capture.resolve()),
               '--out', str(folder.resolve()), '--samples', str(samples), '--warmup', str(warmup), '--pass', str(selected)]
        for symbol in symbols: cmd += ['--metric', symbol]
        if indices is not None:
            for index in indices: cmd += ['--frame-range', str(index)]
        if mapping is not None: cmd += ['--pass-map', ','.join(map(str, mapping))]
        if weights is not None:
            path = args.out / (label + '-weights.json'); write(path, weights); cmd += ['--weights', str(path.resolve())]
        if experiment: cmd += ['--experiment', str(experiment.resolve())]
        if handshake:
            ready = args.out / (label + '-ready')
            cmd += ['--ready-file', str(ready.resolve())]
            process = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
            try:
                time.sleep(.5)
                check(label + ' waits before opening output', process.poll() is None and not folder.exists())
                ready.touch()
                stdout, stderr = process.communicate(timeout=180)
                result = subprocess.CompletedProcess(cmd, process.returncode, stdout, stderr)
            finally:
                if process.poll() is None:
                    process.kill(); process.communicate()
        else:
            result = subprocess.run(cmd, env=env, capture_output=True, text=True, encoding='utf-8', timeout=180)
        (args.out / (label + '.log')).write_text(result.stdout + '\n' + result.stderr, encoding='utf-8')
        check(label + ' process succeeds', result.returncode == 0)
        profile = read(folder / 'scheduled-profile.json')
        check(label + ' requested warmup retained', profile['warmup_count'] == warmup)
        publisher = load_scheduled_result(folder, profile)
        check(label + ' original result reader accepts all sidecars and numeric values', isinstance(publisher, dict))
        progress = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{"image_matches":')]
        check(label + ' one progress event per replay', len(progress) == len(profile['replays']))
        for index, event in enumerate(progress):
            check(label + f' progress {index}', event == dict(replay=index, set=profile['replays'][index]['set'],
                ranges=len(profile['selection']['ranges']), image_matches=True))
        actual_pass = int(selected) if selected != 'all' else 0xffffffff
        if mapping and actual_pass != 0xffffffff: actual_pass = mapping[actual_pass]
        iterations = samples if actual_pass == 0 else 1
        expected_replays = (0 if weights else 1) + (len(profile['plan']['passes']) if actual_pass == 0xffffffff else iterations)
        check(label + ' iteration and replay counts', profile['actual_iteration_count'] == iterations and len(profile['replays']) == expected_replays)
        count = len(profile['selection']['ranges'])
        check(label + ' source report count', len(profile['records']) == count * expected_replays == len(publisher['records']))
        if not experiment:
            with Frame(capture) as frame:
                check(label + ' original FrameFile selection', select_frame_ranges(frame, indices) == profile['selection'])
        for image_path in [folder / 'baseline.png'] + [folder / f'replay-{i:03d}/output.png' for i in range(expected_replays)]:
            with Image.open(image_path) as image:
                check(label + ' PNG contains measured baseline bytes ' + image_path.parent.name,
                      hashlib.sha256(image.convert('RGBA').tobytes()).hexdigest() == profile['baseline_rgba_sha256'])
        check(label + ' all raw report hashes and sizes', all(sha(folder / row['raw_report']) == row['raw_sha256']
            and (folder / row['raw_report']).stat().st_size == row['raw_size'] for row in profile['records']))
        exported = list(csv.DictReader((folder / 'scheduled-metrics.csv').open(encoding='utf-8-sig', newline='')))
        check(label + ' CSV row count', len(exported) == len(profile['metrics']))
        for row, cell in zip(exported, profile['metrics'], strict=True):
            check(label + ' CSV identity', int(row['range_index']) == cell['range_index'] and row['metric'] == cell['metric'])
            for key in ('median', 'minimum', 'maximum', 'mean', 'variation_percent'):
                check(label + ' CSV scalar ' + key, row[key] == '' if cell[key] is None else float(row[key]) == cell[key])
        audit = read(folder / 'scheduler-audit.json')['adapter']
        check(label + ' resources released', audit['owned'] == audit['native_pool']['cached'] == audit['local_lock_depth'] == 0
              and not audit['subscriptions'] and audit['probes_created'] == 1)
        check(label + ' frozen source identity', profile['frame_sha256'] == sha(capture))
        check(label + ' native runtime only', not profile['production_gpa_dependency'] and not any(
              'intelswtools' in m.lower() or Path(m).name.lower().startswith('python') for m in profile['loaded_modules']))
        check(label + ' source-compatibility limit text', len(profile['limits']) == 8 and profile['complete_original_scheduling'] is False)
        profiles.append(dict(path=str(folder), replays=expected_replays, reports=len(profile['records']), sha256=sha(folder / 'scheduled-profile.json')))
        print(label, 'PASS', expected_replays, 'replays', len(profile['records']), 'reports', flush=True)
        return profile

    run('gf2-all-ranges', indices=None)
    run('gf2-repeat-zero', selected=0, samples=3, mapping=[], warmup=1)
    nonzero = run('gf2-nonzero', selected=1, samples=4)
    check('nonzero pass leaves columns unmeasured', any(not c['measured'] and c['weight'] is None for c in nonzero['metrics']))
    run('gf2-mapped', selected=0, samples=4, mapping=[1, 0])
    cached = run('gf2-cached', selected=0, samples=2, weights=[0., 2., 4.])
    check('zero cached weight retains zero samples', all(v in (0., None) for c in cached['metrics'] if c['range_index'] == 0 for v in c['values']))
    run('bf1-worker', capture=bf, indices=(2, 0, 1), worker=True, handshake=True)
    with Frame(gf) as frame:
        draw = next(e.id for e in frame.entries.values() if e.category == 7 and 0x35 <= e.type <= 0x3d)
        project = dict(format='FloraGPA experiment 1', frame_sha256=hashlib.sha256(frame.bytes).hexdigest(), frame_name=gf.name,
            cursor=1, history=[dict(label='Disable first draw', operations=[dict(kind='enabled', event=draw, value=False)])])
    experiment = args.out / 'disabled-experiment.json'; write(experiment, project)
    changed = run('gf2-experiment', selected=0, samples=1, weights=[1., 1., 1.], experiment=experiment)
    check('frozen experiment retained', changed['experiment']['sha256'] == sha(experiment) and changed['experiment']['cursor'] == 1)
    check('selection keeps disabled draw', any(c['event'] == draw and c['enabled'] is False for r in changed['selection']['ranges'] for c in r['commands']))

    failures = [(['--samples', '0', '--pass', '0'], True), (['--samples', '101'], False),
                (['--pass', '99'], True), (['--frame-range', '0', '--frame-range', '0'], False),
                (['--warp'], False), (['--metric', symbols[0]], False)]
    for i, (extra, audit_expected) in enumerate(failures):
        folder = args.out / f'reject-{i}'
        cmd = [str(args.exe.resolve()), 'metric-iterations', str(gf.resolve()), '--out', str(folder.resolve()),
               '--warmup', '0', '--metric', symbols[0], '--frame-range', '0'] + extra
        result = subprocess.run(cmd, env=env, capture_output=True, text=True, encoding='utf-8', timeout=90)
        check(f'reject {i} fails', result.returncode != 0)
        check(f'reject {i} has no success profile', not (folder / 'scheduled-profile.json').exists())
        if audit_expected:
            audit = read(folder / 'scheduler-audit.json')
            check(f'reject {i} audit and cleanup', audit['failure'] is not None and audit['adapter']['local_lock_depth'] == 0
                  and audit['adapter']['owned'] == 0 and audit['adapter']['priority_mutex']['closed'])
    # A directory cannot grant the desktop job handshake; neither frame nor output is opened on timeout.
    ready_directory = args.out / 'ready-directory'; ready_directory.mkdir()
    timeout_output = args.out / 'ready-timeout-output'
    start = time.monotonic()
    result = subprocess.run([str(args.worker.resolve()), 'metric-iterations', 'not-opened.gpa_frame',
        '--out', str(timeout_output.resolve()), '--metric', symbols[0], '--ready-file', str(ready_directory.resolve())],
        env=env, capture_output=True, text=True, encoding='utf-8', timeout=30)
    elapsed = time.monotonic() - start
    check('ready handshake times out before reading capture', result.returncode != 0 and
        'Scheduled collector was not assigned to the desktop process job' in result.stdout + result.stderr and
        10 <= elapsed < 25 and not timeout_output.exists())
    summary = dict(passed=True, checks=len(checks), request_cases=len(requests), collections=len(profiles),
                   replays=sum(r['replays'] for r in profiles), raw_reports=sum(r['reports'] for r in profiles),
                   profiles=profiles, checks_detail=checks, exe_sha256=sha(args.exe), worker_sha256=sha(args.worker),
                   reference_sha256={name: sha(args.reference / 'standalone' / name) for name in ('md_iterations.py', 'md_iteration_results.py')})
    write(args.out / 'validation.json', summary)
    print(json.dumps({k: v for k, v in summary.items() if k not in ('profiles', 'checks_detail', 'reference_sha256')}), flush=True)


if __name__ == '__main__': main()
