"""Development-only persistent Qt recovery soak against a relocated release package.

One test process owns one MainWindow for the whole run. GPU workers are serial.
Progress snapshots are immutable; the final journal exists only on completion.
Do not resume or replace
a failed run's directory. Resource observations are evidence, not a leak-free proof.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--test-exe', type=Path, required=True)
    parser.add_argument('--qt-test-dll', type=Path, required=True)
    parser.add_argument('--captures', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=1800)
    parser.add_argument('--pairs', type=int, default=20)
    args = parser.parse_args()
    if not 0 <= args.seconds <= 86400 or not 1 <= args.pairs <= 10000:
        parser.error('seconds must be 0..86400 and pairs must be 1..10000')
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    report = dict(schema='FloraGPA persistent Qt soak runner 1', completed=False,
                  passed=False, minimum_seconds=args.seconds, minimum_pairs=args.pairs,
                  scope='Same-host offscreen Qt window, system-only PATH, serial production workers; not clean-machine or all-workflow certification')

    def save():
        temporary = root / 'validation.pending.json'
        temporary.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        temporary.replace(root / 'validation.json')

    save()
    try:
        repo = Path(__file__).resolve().parents[1]
        manifests = json.loads((repo / 'docs/capture-corpus.json').read_text(encoding='utf-8'))['cases']
        wanted = [case for case in manifests if case['family'] in ['gf2', 'bf1']]
        assert len(wanted) == 2
        report['captures'] = []
        for case in wanted:
            path = (args.captures / case['path']).resolve(strict=True)
            actual = digest(path)
            assert actual == case['sha256'], f'Capture identity mismatch: {path}'
            report['captures'].append(dict(path=str(path), sha256=actual,
                                            reference_rgba_sha256=case['reference_rgba_sha256']))
        package = args.package.resolve(strict=True)
        report['package'] = str(package)
        report['package_files'] = {p.relative_to(package).as_posix(): digest(p)
                                   for p in sorted(package.rglob('*')) if p.is_file()}
        report['sources'] = {}
        for name in ['tests/RecoveryUiTests.cpp', 'tests/RecoveryJournal.h',
                     'tests/HeapRetentionProbe.h', 'tools/validate_recovery_soak.py']:
            frozen = root / 'sources' / name
            frozen.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(repo / name, frozen)
            report['sources'][name] = digest(frozen)
        runtime = root / 'runtime'
        shutil.copytree(package, runtime)
        executable = runtime / 'FloraRecoveryUiTests.exe'
        shutil.copy2(args.test_exe.resolve(strict=True), executable)
        shutil.copy2(args.qt_test_dll.resolve(strict=True), runtime / 'Qt6Test.dll')
        report['test_exe_sha256'] = digest(executable)
        report['qt_test_sha256'] = digest(runtime / 'Qt6Test.dll')
        env = {k: v for k, v in os.environ.items() if k.upper() in
               {'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA'}}
        windows = Path(os.environ['WINDIR'])
        env.update(PATH=str(windows / 'System32') + os.pathsep + str(windows),
                   QT_QPA_PLATFORM='offscreen',
                   QTEST_FUNCTION_TIMEOUT=str((args.seconds + args.pairs * 180 + 600) * 1000),
                   FLORA_TEST_CAPTURE_DIR=str(args.captures.resolve()),
                   FLORA_RECOVERY_MIN_SECONDS=str(args.seconds),
                   FLORA_RECOVERY_ITERATIONS=str(args.pairs),
                   FLORA_RECOVERY_JOURNAL=str(root / 'journal.json'))
        report['test_environment'] = {k: v for k, v in env.items() if k.startswith(('FLORA_', 'QT')) or k == 'PATH'}
        command = [str(executable), 'originalCaptureRecovery', '-o', str(root / 'qt-results.txt') + ',txt']
        report['command'] = command
        save()
        start = time.monotonic()
        with (root / 'process.log').open('wb') as log:
            result = subprocess.run(command, cwd=runtime, env=env, stdout=log,
                                    stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW,
                                    timeout=args.seconds + args.pairs * 180 + 900)
        report.update(exit_code=result.returncode, process_seconds=time.monotonic() - start)
        save()
        assert result.returncode == 0, 'Qt recovery test failed; inspect qt-results.txt and journal.json.progress'
        text = (root / 'qt-results.txt').read_text(encoding='utf-8')
        assert 'Totals: 3 passed, 0 failed, 0 skipped' in text, 'Missing complete Qt result'
        journal = json.loads((root / 'journal.json').read_text(encoding='utf-8'))
        assert journal['completed'] and journal['phase'] == 'complete'
        assert journal['elapsed_ms'] >= args.seconds * 1000
        rows = journal['observations']
        assert len(rows) == journal['completed_cycles'] and len(rows) >= args.pairs * 2 and len(rows) % 2 == 0
        for index, row in enumerate(rows):
            case = wanted[index % 2]
            assert row['capture'] == Path(case['path']).name and row['iteration'] == index // 2
            assert row['rgba_sha256'] == case['reference_rgba_sha256']
        report.update(completed=True, passed=True, cycles=len(rows), elapsed_ms=journal['elapsed_ms'],
                      journal_sha256=digest(root / 'journal.json'), qt_result_sha256=digest(root / 'qt-results.txt'))
        print(json.dumps({k: report[k] for k in ['passed', 'cycles', 'elapsed_ms']}, indent=2), flush=True)
    except BaseException as error:
        report['error'] = str(error)
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
