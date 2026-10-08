"""Development-only CPU check of API search against every registered capture.

Uses the unchanged commandMatches implementation as its search oracle. This is
index/load compatibility evidence, not GPU execution or original-player parity.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re

from validate_source_build import run_process


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--test-exe', type=Path, required=True)
    parser.add_argument('--qt-bin', type=Path, required=True)
    parser.add_argument('--legacy-root', type=Path, required=True)
    parser.add_argument('--artifacts-root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--suites', type=Path, default=Path('docs/m4-gate-suites.json'))
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    report = dict(completed=False, passed=False, scope='CPU search projection and capture loading only')
    try:
        config = json.loads(args.suites.read_text(encoding='utf-8'))
        assert config['manifest_hash_policy'] == 'UTF-8 bytes with CRLF normalized to LF'
        roots = dict(legacy=args.legacy_root.resolve(strict=True), artifacts=args.artifacts_root.resolve(strict=True))
        captures, manifests, registrations = {}, {}, 0
        for suite in config['suites']:
            manifest = repo / suite['manifest']
            data = manifest.read_bytes()
            assert hashlib.sha256(data.replace(b'\r\n', b'\n')).hexdigest() == suite['manifest_sha256']
            manifests[suite['manifest']] = digest(manifest)
            for case in json.loads(data)['cases']:
                path = (roots[suite['capture_root']] / suite['subdirectory'] / case['path']).resolve(strict=True)
                capture = captures.setdefault(case['sha256'], dict(path=str(path), sha256=case['sha256'], registrations=[]))
                capture['registrations'].append(suite['id'] + '/' + case['id'])
                registrations += 1
        assert registrations == config['expected_cases']
        assert len(captures) == config['expected_unique_captures']
        inventory = root / 'captures.json'
        inventory.write_text(json.dumps(list(captures.values()), indent=2) + '\n', encoding='utf-8')
        executable = args.test_exe.resolve(strict=True)
        env = dict(os.environ, FLORA_COMMAND_SEARCH_CORPUS=str(inventory))
        env['PATH'] = str(args.qt_bin.resolve(strict=True)) + os.pathsep + env.get('PATH', '')
        results = root / 'qt-results.txt'
        command = [str(executable), 'original', '-o', str(results) + ',txt']
        report.update(command=command, test_exe_sha256=digest(executable), registrations=registrations,
                      captures=len(captures), manifests=manifests, inventory_sha256=digest(inventory),
                      suites_sha256=digest(args.suites))
        with (root / 'process.log').open('wb') as log:
            code = run_process(command, repo, env, log, timeout=1800)
        report['exit_code'] = code
        assert code == 0, 'Search test failed; see qt-results.txt'
        text = results.read_text(encoding='utf-8')
        totals = re.findall(r'Totals: (\d+) passed, (\d+) failed, (\d+) skipped', text)
        assert totals == [(str(len(captures) + 2), '0', '0')], totals
        measurements = re.findall(r'API search records (\d+) matching comparisons (\d+) preparation ms (\d+)', text)
        assert len(measurements) == len(captures)
        report.update(records=sum(int(r[0]) for r in measurements), comparisons=sum(int(r[1]) for r in measurements),
                      qt_results_sha256=digest(results), completed=True, passed=True)
    finally:
        (root / 'validation.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
