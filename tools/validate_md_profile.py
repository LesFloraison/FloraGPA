"""Development oracle for the native uniform Intel metric collector."""
import argparse
import csv
import hashlib
import io
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
from unittest.mock import patch
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('reference', 'exe', 'probe-exe', 'qt-bin', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--reuse-completed-from', type=Path,
                        help='Revalidate previously completed collections; rerun missing/failed cases into the new output')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.reference / 'standalone'))
    import md_profile
    from frame import Frame
    from experiments import Experiment
    from gtpin_experiments import submissions
    from md_frame_ranges import select_frame_ranges
    from md_interval import selection
    from metric_values import summarize_records
    from metric_passes import profile_matrix
    from metric_planner import plan_metrics, requested_results
    from md_publisher_values import load_publisher_result, publisher_analysis
    from md_acquisition_priority import validate_priority_result
    from devices import create_vendor_device
    from metrics_discovery import Metrics
    env = {k: v for k, v in os.environ.items() if k.upper() in
           {'SYSTEMROOT', 'SYSTEMDRIVE', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA'}}
    env['PATH'] = str(args.qt_bin.resolve()) + ';' + os.environ['SystemRoot'] + '/System32;' + os.environ['SystemRoot']
    checks = []
    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        if not passed:
            raise AssertionError(name)
    def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
    def sha(data): return hashlib.sha256(data).hexdigest()
    def write(path, value): path.write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8')
    def csv_equal(path, fields, rows):
        stream = io.StringIO(newline='')
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader(); writer.writerows(rows)
        check(path.name + ' exact CSV', path.read_bytes() == b'\xef\xbb\xbf' + stream.getvalue().encode('utf-8'))

    requests = [{}]
    for key in ('samples', 'warmup'):
        requests += [{key: value} for value in (0, 1, 100, -1, 101, True, 1., '3', None)]
    requests += [dict(sets=value) for value in (None, [], ['RenderBasic'], ['A', 'A'])]
    requests += [dict(symbols=['GpuTime']), dict(symbols=['GpuTime'], sets=[]),
                 dict(interval=[None, None]), dict(interval=[1]), dict(interval='all'),
                 dict(interval=[1, 2], events=[1]), dict(frame_ranges='all', events=[1]),
                 dict(frame_ranges=[0], interval=[1, 2]), dict(frame_ranges='all'),
                 dict(events=[113, 113]), dict(publisher_values=True)]
    class AtDevice(Exception): pass
    expected = []
    for i, request in enumerate(requests):
        cfg = dict(samples=1, warmup=1, sets=['RenderBasic'], symbols=None, events=[],
                   interval=None, frame_ranges=None, publisher_values=False)
        cfg.update(request)
        try:
            with patch.object(md_profile, 'create_vendor_device', side_effect=AtDevice):
                md_profile.collect('not-opened.gpa_frame', args.out / f'request-{i}',
                    sets=tuple(cfg['sets']) if cfg['sets'] is not None else None,
                    events=cfg['events'], interval=cfg['interval'], samples=cfg['samples'], warmup=cfg['warmup'],
                    metric_symbols=cfg['symbols'], frame_ranges=cfg['frame_ranges'], publisher_values=cfg['publisher_values'])
        except AtDevice: expected.append(cfg)
        except Exception as exc: expected.append(dict(error=str(exc)))
    write(args.out / 'requests.json', requests)
    subprocess.run([str(args.probe_exe.resolve()), '--probe', str((args.out / 'requests.json').resolve()),
                    str((args.out / 'request-results.json').resolve())], env=env, check=True, timeout=30)
    check('request domain', read(args.out / 'request-results.json') == expected)

    gf = args.reference / 'GF2_Exilium_2026_03_03__00_19_35.gpa_frame'
    bf = args.reference / 'bf1_2026_01_21__16_53_05.gpa_frame'
    bridge = args.exe.resolve().parent / 'FloraGPA.Metrics.dll'
    experiment_path = args.out / 'experiment.json'
    with Frame(gf) as frame:
        write(experiment_path, dict(format='FloraGPA experiment 1', frame_sha256=sha(frame.bytes),
              frame_name=gf.name, cursor=1, history=[dict(label='Disable first draw',
              operations=[dict(kind='enabled', event=113, value=False)])]))
    stats_fields = ['set','event','start_event','end_event','metric','label','unit','total_samples',
                    'valid_samples','invalid_samples','median','minimum','maximum','mean','variation_percent']
    request_fields = ['metric','label','unit','set','event','start_event','end_event','pass_index',
                      'sample_index','value','available','value_type']
    cases = [
        ('events', gf, ['--event','113','--event','181','--samples','2'], False),
        ('events-publisher', gf, ['--event','113','--event','181','--samples','2'], True),
        ('interval', gf, ['--interval','--start-event','79','--end-event','113','--samples','2'], False),
        ('interval-publisher', gf, ['--interval','--start-event','79','--end-event','113','--samples','2'], True),
        ('ranges', gf, ['--frame-range','2','--frame-range','0','--samples','2'], False),
        ('ranges-requested', gf, ['--frame-range','2','--frame-range','0','--samples','2',
                                '--metric','GpuTime','--metric','Sampler00InputAvailable','--metric','Sampler00OutputReady'], True),
        ('all-events', gf, ['--warmup','1'], False),
        ('full-interval', gf, ['--interval'], True),
        ('experiment-ranges', gf, ['--frame-range','0','--frame-range','2','--experiment',str(experiment_path.resolve())], True),
        ('all-ranges', gf, ['--all-frame-ranges'], True),
        ('all-sets', gf, ['--all-sets','--frame-range','0'], False),
        ('bf1-ranges', bf, ['--frame-range','0','--frame-range','100','--set','RenderBasic'], True),
    ]
    profiles = []
    reused = []
    for name, capture, options, publisher in cases:
        folder = args.out / name
        previous = args.reuse_completed_from / name if args.reuse_completed_from else None
        cmd = [str(args.exe.resolve()), 'metric-profile', str(capture.resolve()), '--out', str(folder.resolve()),
               '--warmup', '0'] + options + (['--publisher-values'] if publisher else [])
        if previous is not None and (previous / 'profile.json').is_file():
            shutil.copytree(previous, folder)
            reused.append(str(previous.resolve()))
            (args.out / (name + '.log')).write_text('Revalidated completed collection: ' + str(previous.resolve()), encoding='utf-8')
        else:
            process = subprocess.run(cmd, env=env, capture_output=True, timeout=300)
            (args.out / (name + '.log')).write_bytes(process.stdout + process.stderr)
            check(name + ' exits', process.returncode == 0)
        p = read(folder / 'profile.json'); profiles.append((folder, p))
        check(name + ' modules', not any(Path(m).name.lower().startswith(('python','gpa-','gpa_','tk8','tcl8')) for m in p['loaded_modules']))
        check(name + ' validation', all(p['validation'][k] is True for k in ('frame_matches','experiment_matches','image_matches','event_mapping_valid')))
        validate_priority_result(folder, p)
        check(name + ' schedule', [(r['set'], r['sample_index']) for r in p['validation']['passes']] ==
              [(s['name'], i) for s in p['sets'] for i in range(p['sample_count'])])
        base = Image.open(folder / 'baseline.png').convert('RGBA')
        check(name + ' baseline', sha(base.tobytes()) == p['baseline_rgba_sha256'])
        with Frame(capture) as frame:
            check(name + ' source hash', sha(frame.bytes) == p['frame_sha256'])
            edits = Experiment(frame, folder / 'experiment.json') if p['experiment'] is not None else None
            effective = edits.frame_for(frame) if edits else frame
            available = {r['event']: r for r in submissions(frame, edits) if r['enabled']}
            if p['selection_mode'] == 'frame_ranges':
                expected_scope = select_frame_ranges(effective, [r['range_index'] for r in p['frame_ranges']['ranges']], edits)
                check(name + ' scope', p['frame_ranges'] == expected_scope)
            elif p['selection_mode'] == 'interval':
                check(name + ' scope', p['interval'] == selection(effective, edits, p['interval']['start_event'], p['interval']['end_event']))
            for row in p['records']:
                if row['event'] is not None:
                    check(name + ' event identity', all(row[k] == available[row['event']][k] for k in ('api','shaders')))
        for v in p['validation']['passes']:
            sub = folder / f'pass-{v["pass_index"]:02d}'
            image = Image.open(sub / 'output.png').convert('RGBA')
            check(name + ' replay image', image.size == base.size and image.tobytes() == base.tobytes())
            raw = read(sub / 'raw-results.json')
            check(name + ' pass sidecar', raw['validation'] == v and raw['records'] == [r for r in p['records'] if r['pass_index']==v['pass_index']])
        check(name + ' matrix', profile_matrix(p) == read(folder / 'metric-iterations.json')['sets'])
        statistics = summarize_records(p)
        check(name + ' statistics', statistics == p['sample_statistics'] == read(folder / 'sample-statistics.json'))
        csv_equal(folder / 'sample-statistics.csv', stats_fields, statistics)
        if 'metric_request' in p:
            check(name + ' plan', plan_metrics(read(folder / 'catalog.json'), p['metric_request']['requested_metrics']) == p['metric_request'])
            requested = requested_results(p, p['metric_request'])
            check(name + ' requested', read(folder / 'requested-metrics.json') == requested)
            csv_equal(folder / 'requested-metrics.csv', request_fields, requested['samples'])
        metric_rows = []
        metadata = {s['name']:s for s in p['sets']}
        for r in p['records']:
            for d,v in zip(metadata[r['set']]['metrics'], r['values'], strict=True):
                valid = r['available'] and v['value'] is not None
                metric_rows.append(dict(event=r['event'], api=r['api'], **{'pass':r['pass_index']}, set=r['set'],
                    metric=d['name'], label=d['label'], unit=d['unit'], value=v['value'] if valid else None,
                    available=valid, value_type=v['type'], scope=p['selection_mode'],
                    start_event=r.get('start_event'), end_event=r.get('end_event'), command_count=r.get('command_count'), sample_index=r['sample_index']))
        csv_equal(folder / 'metrics.csv', ['event','api','pass','set','metric','label','unit','value','available','value_type','scope','start_event','end_event','command_count','sample_index'], metric_rows)
        if publisher:
            pub = load_publisher_result(folder, p); analysis = publisher_analysis(p, pub)
            check(name + ' publisher statistics', analysis['statistics'] == read(folder / 'publisher-statistics.json'))
            check(name + ' publisher matrix', analysis['iterations'] == read(folder / 'publisher-iterations.json')['sets'])
            csv_equal(folder / 'publisher-statistics.csv', stats_fields, analysis['statistics'])
            publisher_rows = []
            for r in pub['records']:
                for v in r['values']:
                    publisher_rows.append(dict(**{k:r[k] for k in ('set','event','start_event','end_event','pass_index','sample_index','key')},
                        metric=v['name'],unit=v['unit'],value=v['value'] if r['available'] else None,available=r['available']))
            csv_equal(folder / 'publisher-values.csv', ['set','event','start_event','end_event','pass_index','sample_index','key','metric','unit','value','available'], publisher_rows)
            if 'requested' in analysis:
                check(name + ' publisher request', analysis['requested'] == read(folder / 'publisher-requested-metrics.json'))
                csv_equal(folder / 'publisher-requested-metrics.csv', request_fields, analysis['requested']['samples'])
        device = create_vendor_device(0x8086)
        try:
            with Metrics(device, bridge) as md:
                for row in p['records']:
                    md.select(row['set']); raw = (folder / row['raw_report']).read_bytes()
                    decoded, returned = md.decode(raw)
                    check(name + ' raw hash', sha(raw) == row['raw_sha256'] and len(raw) == row['raw_size'])
                    check(name + ' original decoder', returned == raw and decoded['values'] == row['values'] + row['information']
                          and decoded['available'] == row['available'] and decoded['unavailable_reasons'] == row['unavailable_reasons'])
        finally: device.close()
        print(name, 'PASS', flush=True)

    # Independently run the original owner, then compare invariant profile fields.
    original = md_profile.collect(gf, args.out / 'python-owner', sets=('RenderBasic',), events=(113,181),
                                  samples=2, warmup=0, publisher_values=True, bridge=bridge)
    native = profiles[1][1]
    for key in ('schema_version','arbitration','priority_audit','sample_count','warmup_count','sample_schedule',
                'statistics_precision','backend','frame_sha256','adapter','catalog_version','adapter_luid','provenance',
                'production_gpa_dependency','shader_instrumentation','baseline_rgba_sha256','baseline_matches',
                'baseline_scope','experiment','selected_events','selection_mode','interval','frame_ranges','sets','limits'):
        if key == 'provenance':
            check('Python owner binary provenance', all(
                native[key][part]['sha256'] == original[key][part]['sha256'] and
                Path(native[key][part]['path']).resolve() == Path(original[key][part]['path']).resolve()
                for part in ('bridge', 'driver')))
        else:
            check('Python owner ' + key, native[key] == original[key])
    check('Python owner record identities', [{k:r[k] for k in ('event','api','shaders','set','pass_index','sample_index','raw_report')}
          for r in native['records']] == [{k:r[k] for k in ('event','api','shaders','set','pass_index','sample_index','raw_report')}
          for r in original['records']])
    result = dict(passed=True, requests=len(requests), collections=len(profiles), reused_collections=reused,
                  replays=sum(len(p['validation']['passes']) for _,p in profiles),
                  reports=sum(len(p['records']) for _,p in profiles), checks=checks)
    write(args.out / 'validation.json', result)
    print(json.dumps({k:v for k,v in result.items() if k!='checks'}), flush=True)


if __name__ == '__main__':
    main()
