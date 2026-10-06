"""Verify original predicate captures, wire observations and isolated inspection boundaries.

Development only. Original-player comparisons are supplied by validate_corpus.py;
an observed normalization truth table is not a promise about every GPA version.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def records(path):
    raw = path.read_bytes()
    assert raw[:4] == b'IGPA' and struct.unpack_from('<II', raw, 4) == (0x128, 3)
    count, = struct.unpack_from('<I', raw, 12)
    table, = struct.unpack_from('<Q', raw, 0xf4)
    assert table + count*24 <= len(raw)
    result = {}
    for i in range(count):
        identity, offset, size, flags, category, kind = struct.unpack_from('<QQIBBH', raw, table+i*24)
        assert identity not in result and 0x128 <= offset <= offset+size <= table
        result[identity] = (category, kind, raw[offset:offset+size])
    return result


def main():
    p = argparse.ArgumentParser(__doc__)
    for name in ('captures', 'comparison', 'exe', 'qt-bin', 'out'):
        p.add_argument('--'+name, type=Path, required=True)
    a = p.parse_args()
    root, exe, out = a.captures.resolve(), a.exe.resolve(strict=True), a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    manifest = load(root/'manifest.json')
    comparison = load(a.comparison/'validation.json')
    assert manifest['completed'] and len(manifest['cases']) == 16
    for name, digest in manifest['producer_files'].items():
        assert sha(root/'producer'/name) == digest
    report = dict(completed=False, exe_sha256=sha(exe), manifest_sha256=sha(root/'manifest.json'),
                  comparison_sha256=sha(a.comparison/'validation.json'), cases=[])
    env = dict(os.environ)
    env.pop('GPA_LOCAL_INJECT', None)
    env['PATH'] = str(a.qt_bin.resolve())+os.pathsep+env['PATH']

    def save():
        (out/'validation.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')

    try:
        for mode, case in enumerate(manifest['cases']):
            capture = root/case['path']
            assert sha(capture) == case['sha256']
            entries = records(capture)
            setters = [(i, struct.unpack('<QQQI', raw)) for i, (cat, kind, raw) in sorted(entries.items())
                       if cat == 7 and kind == 0x248 and struct.unpack_from('<Q', raw, 16)[0]]
            assert len(setters) == 1
            event, (_, context, resource, saved) = setters[0]
            refresh, present = mode//4 == 1, mode//4 in (1, 3)
            visible, comparison_value = bool(mode & 1), bool(mode & 2)
            assert saved == (comparison_value if refresh else visible != comparison_value)
            assert (resource in entries) == present
            if present:
                assert entries[resource][:2] == (5, 0x96) and len(entries[resource][2]) == 24
            measured = next(c for c in comparison['cases'] if c['id'] == case['id'])
            if present:
                assert measured['status'] == measured['original_status'] == 'repeat_stable'
                assert measured['original_comparison_status'] == 'observed_equal'
                assert all(r['report']['rgba_sha256'] == case['reference_rgba_sha256'] for r in measured['native'])
            else:
                assert measured['status'] == 'replay_failed'
                assert measured['original_status'] == 'replay_or_export_failed'
                for n in (1, 2):
                    worker = (a.comparison/case['id']/f'original-{n}'/'worker.log').read_text(errors='replace')
                    assert 'Open failed: status=13' in worker
            row = dict(id=case['id'], resource=resource, first_set=event, descriptor_present=present,
                       producer_query_value=visible, producer_comparison=comparison_value,
                       saved_comparison=saved, captured_interval_before_use=refresh, inspections=[])
            report['cases'].append(row)
            if present:
                ends = [i for i, (cat, kind, raw) in entries.items() if cat == 7 and kind == 0x243]
                boundaries = [('first_use', event, 'ready' if refresh else 'replay_baseline'),
                              ('completed_interval', max(ends), 'ready')]
                for warp in (False, True):
                    for label, at, status in boundaries:
                        folder = out/f'{mode}-{label}-{"warp" if warp else "hardware"}'
                        command = [str(exe), 'predicate', str(capture), '--event', str(at),
                                   '--id', str(resource), '--out', str(folder)] + (['--warp'] if warp else [])
                        run = subprocess.run(command, env=env, capture_output=True, timeout=60)
                        (out/(folder.name+'.log')).write_bytes(run.stdout+run.stderr)
                        assert run.returncode == 0
                        result = load(folder/'predicate.json')
                        assert result['status'] == status and result['captured_result_restored'] is False
                        assert result['value'] == (visible if status == 'ready' else None)
                        assert result['source'] == ('replayed_gpu_query' if status == 'ready' else 'native_player_empty_begin_end')
                        row['inspections'].append(dict(event=at, warp=warp, status=status, value=result['value']))
            save()
        report['completed'] = True
    finally:
        save()


if __name__ == '__main__':
    main()
