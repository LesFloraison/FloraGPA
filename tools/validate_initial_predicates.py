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
    p.add_argument('--recovered-conditions', action='store_true',
                   help='Require proof-backed condition replay for missing-descriptor originals')
    a = p.parse_args()
    root, exe, out = a.captures.resolve(), a.exe.resolve(strict=True), a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    manifest = load(root/'manifest.json')
    comparison = load(a.comparison/'validation.json')
    assert manifest['completed'] and len(manifest['cases']) == 16
    for name, digest in manifest['producer_files'].items():
        assert sha(root/'producer'/name) == digest
    report = dict(completed=False, exe_sha256=sha(exe), validator_sha256=sha(Path(__file__)),
                  manifest_sha256=sha(root/'manifest.json'),
                  comparison_sha256=sha(a.comparison/'validation.json'),
                  recovered_conditions=a.recovered_conditions, cases=[])
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
            observations = []
            for i, (cat, kind, raw) in sorted(entries.items()):
                if cat == 7 and kind in (0x30b4, 0x31b4, 0x331d, 0x33e3, 0x34fb):
                    assert len(raw) == 41
                    link, owner, hr, query, has_word, word, size, flags = struct.unpack('<QQiQBIII', raw)
                    assert link == 0 and owner == context and query == resource
                    assert has_word == 1 and size == 4 and flags == 0
                    observations.append(dict(event=i, hresult=hr, captured_word=word))
            assert bool(observations) == (mode//4 in (1, 2))
            if observations:
                assert any(o['hresult'] == 0 and bool(o['captured_word']) == visible for o in observations)
            measured = next(c for c in comparison['cases'] if c['id'] == case['id'])
            if present:
                assert measured['status'] == measured['original_status'] == 'repeat_stable'
                assert measured['original_comparison_status'] == 'observed_equal'
                assert all(r['report']['rgba_sha256'] == case['reference_rgba_sha256'] for r in measured['native'])
            else:
                assert measured['status'] == ('repeat_stable' if a.recovered_conditions else 'replay_failed')
                assert measured['original_status'] == 'replay_or_export_failed'
                if a.recovered_conditions:
                    assert all(r['report']['rgba_sha256'] == case['reference_rgba_sha256'] for r in measured['native'])
                for n in (1, 2):
                    worker = (a.comparison/case['id']/f'original-{n}'/'worker.log').read_text(errors='replace')
                    assert 'Open failed: status=13' in worker
            row = dict(id=case['id'], resource=resource, first_set=event, descriptor_present=present,
                       producer_query_value=visible, producer_comparison=comparison_value,
                       saved_comparison=saved, captured_interval_before_use=refresh,
                       captured_get_data=observations, inspections=[])
            report['cases'].append(row)
            if present or a.recovered_conditions:
                ends = [i for i, (cat, kind, raw) in entries.items() if cat == 7 and kind == 0x243]
                boundaries = [('first_use', event, 'captured_condition' if not present else
                               'ready' if refresh else 'replay_baseline')]
                if present:
                    boundaries.append(('completed_interval', max(ends), 'ready'))
                else:
                    witnesses = [(i, struct.unpack('<QQi16sBIQ', raw))
                                 for i, (cat, kind, raw) in entries.items() if cat == 7 and kind == 0x3166
                                 and struct.unpack_from('<Q', raw)[0] == event]
                    assert len(witnesses) == 1
                    witness, fields = witnesses[0]
                    assert fields[1] == resource and fields[2] & 0xffffffff == 0x887a0002
                    assert fields[3].hex() == '00df6068758ab648a15ea89f698bf81f'
                    assert fields[4:6] == (1, 0) and fields[6] != 0
                    row['normalization_witness'] = witness
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
                        assert result['source'] == ('replayed_gpu_query' if status == 'ready' else
                                                    'captured_normalized_predication' if status == 'captured_condition' else
                                                    'native_player_empty_begin_end')
                        if status == 'captured_condition':
                            assert result['resource']['descriptor_available'] is False
                            assert result['condition_allows_execution'] == (visible != comparison_value)
                            assert result['resource']['condition_proofs'] == [dict(event=event, witness_event=witness, captured_value=saved)]
                        row['inspections'].append(dict(event=at, warp=warp, status=status, value=result['value']))
            save()
        report['completed'] = True
    finally:
        save()


if __name__ == '__main__':
    main()
