"""Produce a reviewable, capture-free baseline from retained compatibility evidence."""
import argparse
from collections import Counter
import json
from pathlib import Path
from catalog_captures import digest


def summarize(run, manifest, evidence):
    cases = []
    sources = {c['id']: c for c in manifest['cases']}
    for item in run['cases']:
        source = sources[item['id']]
        preflight = item.get('preflight', {}).get('report') or {}
        cases.append({
            'id': item['id'], 'source_sha256': source['sha256'], 'family': item['family'],
            'native_status': item['status'], 'original_status': item.get('original_status', 'not_run'),
            'preflight_status': preflight.get('status', 'missing'),
            'errors': preflight.get('errors'), 'warnings': preflight.get('warnings'),
            'native_hashes': [r.get('report', {}).get('rgba_sha256') for r in item['native']],
            'original_hashes': [r.get('report', {}).get('replayed_rgba_sha256') for r in item['original']],
            'native_counts': [r.get('report', {}).get('counts') for r in item['native']],
            'native_repeat_difference': item.get('native_repeat_difference'),
            'original_repeat_difference': item.get('original_repeat_difference'),
            'native_original_observed_difference': item.get('native_original_observed_difference'),
            'controls': [{k: v for k, v in c.items() if k != 'runs'} for c in item.get('controls', [])],
            'boundaries': [{k: v for k, v in c.items() if k != 'runs'} for c in item.get('boundaries', [])],
            'evidence_directory': evidence + '/' + item['id'],
        })
    command_rows = [r for r in run['coverage'] if r['category'] == 7]
    return {
        'schema': 'FloraGPA reviewable compatibility baseline 1',
        'profile': run['profile'], 'evidence_directory': evidence,
        'exe_sha256': run['exe_sha256'], 'manifest_sha256': run['manifest_sha256'],
        'sample_files': len(cases), 'unique_capture_hashes': len({c['source_sha256'] for c in cases}),
        'native_outcomes': dict(Counter(c['native_status'] for c in cases)),
        'original_outcomes': dict(Counter(c['original_status'] for c in cases)),
        'preflight_outcomes': dict(Counter(c['preflight_status'] for c in cases)),
        'observed_command_types': len(command_rows),
        'command_handling_inventory': dict(Counter(c['handling'] for c in command_rows)),
        'accepted_capability_percentage': None,
        'acceptance_note': 'Inventory/path classification is not full semantic acceptance; denominator and original evidence remain incomplete.',
        'cross_implementation_equivalence': run['cross_implementation_equivalence'],
        'native_runtime_audits': dict(Counter(r['runtime_dependency_audit'] for c in run['cases'] for r in c['native'])),
        'native_adapters': sorted({r['report']['adapter'] for c in run['cases'] for r in c['native'] if r.get('report', {}).get('adapter')}),
        'installed_video_controllers': [{k: d[k] for k in ('Name', 'DriverVersion')} for d in run.get('installed_video_controllers', [])],
        'cases': cases, 'coverage': run['coverage'], 'repair_queue': run['queue'],
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--evidence', required=True, help='Repository-relative ignored evidence directory')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    run = json.loads(args.run.read_text(encoding='utf-8'))
    manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
    if run['manifest_sha256'] != digest(args.manifest):
        parser.error('Manifest changed since this run; retain its original inventory')
    result = summarize(run, manifest, args.evidence)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
