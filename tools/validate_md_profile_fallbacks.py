"""Exercise real uniform collection with older optional bridge export sets.

The forwarding DLLs only hide optional exports; every counter/report still comes
from the same native bridge and installed Intel driver. They are test fixtures.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ('reference', 'exe', 'qt-bin', 'msvc-bin', 'out'):
        p.add_argument('--' + key, type=Path, required=True)
    a = p.parse_args(); a.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(a.reference / 'standalone'))
    from md_publisher_values import load_publisher_result, publisher_analysis
    from metric_passes import profile_matrix
    import md_profile
    source = a.exe.resolve().parent / 'FloraGPA.Metrics.dll'
    full = a.out.resolve() / 'flora_profile_full.dll'; shutil.copy2(source, full)
    dump = subprocess.check_output([str(a.msvc_bin / 'dumpbin.exe'), '/exports', str(source)], text=True)
    exports = re.findall(r'^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(FloraMd\w+)\s*$', dump, re.M)
    assert len(exports) >= 31
    # Preload the implementation so Python's secure DLL loader can resolve forwarders.
    loaded = ctypes.WinDLL(str(full))
    env = dict(os.environ)
    env['PATH'] = str(a.qt_bin.resolve()) + ';' + str(a.out.resolve()) + ';' + os.environ['SystemRoot'] + '/System32;' + os.environ['SystemRoot']
    capture = a.reference / 'GF2_Exilium_2026_03_03__00_19_35.gpa_frame'
    checks = []
    def read(path): return json.loads(path.read_text('utf-8'))
    for kind, removed in (
        ('samples', {'FloraMdSampleReserve','FloraMdSampleRecycle','FloraMdSampleClearCache','FloraMdSampleInfo','FloraMdSampleStats'}),
        ('drain', {n for n in exports if n.startswith('FloraMdSample')}),
        ('synchronous', {n for n in exports if n.startswith('FloraMdSample')} | {'FloraMdSubmit','FloraMdPoll','FloraMdDiscard'})):
        proxy = a.out.resolve() / (kind + '.dll')
        definition = a.out / (kind + '.def')
        definition.write_text('LIBRARY ' + kind + '\nEXPORTS\n' + ''.join(
            f'  {name}=flora_profile_full.{name}\n' for name in exports if name not in removed), encoding='ascii')
        subprocess.run([str(a.msvc_bin / 'link.exe'), '/NOLOGO', '/DLL', '/NOENTRY', '/MACHINE:X64',
                        '/DEF:' + str(definition.resolve()), '/OUT:' + str(proxy)], check=True, capture_output=True)
        for scope, flags, options in (
            ('event', ['--event','113'], dict(events=(113,))),
            ('interval', ['--interval','--start-event','79','--end-event','113'], dict(interval=(79,113))),
            ('ranges', ['--frame-range','0','--frame-range','2'], dict(frame_ranges=[0,2]))):
            name = kind + '-' + scope; folder = a.out / name
            cmd = [str(a.exe.resolve()), 'metric-profile', str(capture.resolve()), '--out', str(folder.resolve()),
                   '--metrics-bridge', str(proxy), '--publisher-values', '--samples','2','--warmup','0'] + flags
            run = subprocess.run(cmd, env=env, capture_output=True, timeout=90)
            (a.out / (name + '.log')).write_bytes(run.stdout + run.stderr)
            assert run.returncode == 0, name
            native = read(folder / 'profile.json'); pub = load_publisher_result(folder, native)
            assert publisher_analysis(native, pub)['statistics'] == read(folder / 'publisher-statistics.json')
            assert profile_matrix(native) == read(folder / 'metric-iterations.json')['sets']
            original_folder = a.out / (name + '-python')
            original = md_profile.collect(capture, original_folder, bridge=proxy, samples=2, warmup=0,
                                          publisher_values=True, **options)
            reference = load_publisher_result(original_folder, original)
            if kind == 'synchronous':
                assert 'query_drain' not in pub and 'query_drain' not in reference
            else:
                assert pub['query_drain']['mode'] == reference['query_drain']['mode']
                assert pub['query_drain']['counter_limit'] == reference['query_drain']['counter_limit']
                assert len(pub['query_drain']['records']) == len(reference['query_drain']['records'])
            for key in ('selected_events','selection_mode','interval','frame_ranges','sets','sample_count',
                        'sample_schedule','baseline_rgba_sha256','baseline_matches'):
                assert native[key] == original[key], (name,key)
            assert all(native['validation'][k] is True for k in ('frame_matches','experiment_matches','image_matches','event_mapping_valid'))
            assert len(native['records']) == len(original['records'])
            checks.append(dict(case=name, passed=True, records=len(native['records']),
                               mode=pub.get('query_drain',{}).get('mode','synchronous_counter_end')))
            print(name, 'PASS', flush=True)
    (a.out / 'validation.json').write_text(json.dumps(dict(passed=True, checks=checks), indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
