"""Development-only persistent Qt recovery soak against a relocated release package.

One test process owns one MainWindow for the whole run. GPU workers are serial.
Progress snapshots are immutable; the final journal exists only on completion.
Do not resume or replace
a failed run's directory. Resource observations are evidence, not a leak-free proof.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import time

from validate_source_build import run_process

GUI_WORKFLOW_STAGES = ['before_query', 'after_query', 'after_api_cancel', 'after_api_export',
                       'structure_open', 'after_structure_cancel', 'after_contexts_export',
                       'after_lists_export', 'after_structure_close']


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate_platform(requested, journal):
    if requested not in {'offscreen', 'windows'} or journal.get('qt_platform') != requested:
        raise ValueError('Actual Qt platform differs from requested platform')
    if requested == 'windows':
        rows = journal['observations']
        if not rows or not all(row.get('window_visible') and row.get('window_exposed') for row in rows):
            raise ValueError('Native window was not visible and exposed at every observation')
        if not all(row['gdi_objects'] > 0 and row['user_objects'] > 0 for row in rows):
            raise ValueError('Missing native Windows GUI resource observations')


def validate_gui_resources(root, journal):
    expected = {'before_window.json': journal['gui_before_window'],
                'after_window_destroy.json': journal['gui_after_window_destroy']}
    for index, row in enumerate(journal['observations'], 1):
        stages = row.get('workflows', {}).get('gui_stages', [])
        if journal.get('gui_workflow_stages'):
            if [stage['stage'] for stage in stages] != GUI_WORKFLOW_STAGES:
                raise ValueError('GUI workflow stages differ from required operation order')
            previous = 0
            for stage in stages:
                elapsed = stage['elapsed_ms']
                if type(elapsed) is not int or elapsed < previous:
                    raise ValueError('GUI workflow stage times are invalid or unordered')
                previous = elapsed
                expected[f"workflow-{index:06d}-{stage['stage']}.json"] = stage['snapshot']
        elif stages:
            raise ValueError('Unexpected GUI workflow stages')
        expected[f'cycle-{index:06d}.json'] = row['gui_snapshot']
    for key in ['before', 'after_test_history_clear', 'after_log_clear', 'after_pixmap_cache_clear']:
        expected[f'control-{key}.json'] = journal['retention_control'][key]['gui_snapshot']
    if {p.name for p in root.iterdir()} != set(expected):
        raise ValueError('GUI resource snapshot inventory differs from journal')
    files, pid = {}, None
    for name, summary in expected.items():
        path = root / name
        snapshot = json.loads(path.read_text(encoding='utf-8'))
        if (snapshot['schema'] != 'FloraGPA GUI resource snapshot 1' or snapshot['qt_platform'] != 'windows' or
                snapshot['native_windows_complete'] is not True or snapshot['modules_complete'] is not True):
            raise ValueError('Incomplete or wrong-platform GUI resource snapshot')
        if type(snapshot['process_id']) is not int or snapshot['process_id'] <= 0:
            raise ValueError('Invalid GUI snapshot process identity')
        if pid is None:
            pid = snapshot['process_id']
        if snapshot['process_id'] != pid:
            raise ValueError('GUI snapshots belong to different processes')
        for stage in ['before', 'after']:
            for key in ['handles', 'gdi_objects', 'user_objects']:
                value = snapshot[stage][key]
                if type(value) is not int or value < 0:
                    raise ValueError('Invalid GUI resource count')
        for key in ['native_top_windows', 'qt_top_widgets', 'qt_windows', 'modules']:
            if not isinstance(snapshot[key], list):
                raise ValueError('Invalid GUI snapshot inventory')
        if not snapshot['modules']:
            raise ValueError('Missing GUI snapshot modules')
        keys = ['before', 'after', 'native_windows_complete', 'modules_complete']
        if summary != {key: snapshot[key] for key in keys}:
            raise ValueError('GUI resource summary differs from saved snapshot')
        files[name] = digest(path)
    return files


def validate_workflows(root, rows):
    """Audit retained exports, their UI event inventory and per-capture stability."""
    expected = {'api/commands.json', 'api/commands.csv', 'contexts.json', 'command-lists.json'}
    files, baselines = {}, {}
    for index, row in enumerate(rows, 1):
        evidence = row['workflows']
        directory = f'{index:06d}'
        if evidence['directory'] != directory or evidence['cancelled_choosers'] != 2:
            raise ValueError('Workflow cycle or cancelled chooser count mismatch')
        if set(evidence['files']) != expected:
            raise ValueError('Incomplete workflow exports')
        actual = {}
        for name in sorted(expected):
            path = root / directory / name
            actual[name] = dict(bytes=path.stat().st_size, sha256=digest(path))
            if actual[name] != evidence['files'][name]:
                raise ValueError('Workflow export identity mismatch: ' + str(path))
            files[f'{directory}/{name}'] = actual[name]
        commands = json.loads((root / directory / 'api/commands.json').read_text(encoding='utf-8'))
        ids = evidence['query_events']
        if not ids or len(set(ids)) != len(ids) or [r['id'] for r in commands['commands']] != ids:
            raise ValueError('Query UI and exported event inventory differ')
        if commands['frame'] != row['capture'] or commands['filter'] != dict(text='GetData', resource=None):
            raise ValueError('API export capture/filter mismatch')
        with (root / directory / 'api/commands.csv').open(encoding='utf-8-sig', newline='') as stream:
            if [int(r['id']) for r in csv.DictReader(stream)] != ids:
                raise ValueError('CSV and JSON event inventories differ')
        for name in ['contexts.json', 'command-lists.json']:
            document = json.loads((root / directory / name).read_text(encoding='utf-8'))
            if not isinstance(document, dict) or 'error' in document:
                raise ValueError('Structure export did not complete: ' + name)
        baseline = baselines.setdefault(row['capture'], (actual, ids))
        if baseline != (actual, ids):
            raise ValueError('Repeated workflow export differs for ' + row['capture'])
    return files


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--test-exe', type=Path, required=True)
    parser.add_argument('--qt-test-dll', type=Path, required=True)
    parser.add_argument('--captures', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=1800)
    parser.add_argument('--pairs', type=int, default=20)
    parser.add_argument('--platform', choices=['offscreen', 'windows'], default='offscreen',
                        help='Qt platform to exercise; the test must report the same actual platform')
    parser.add_argument('--gui-resources', action='store_true',
                        help='Save current-process window/module/count snapshots and observe after MainWindow destruction')
    parser.add_argument('--workflows', action='store_true',
                        help='Also repeat Query navigation, filtered API export and both structure exports; retain every cycle')
    parser.add_argument('--retention-control', action='store_true',
                        help='Measure heap bytes after separately clearing test history, application log and Qt pixmap cache')
    parser.add_argument('--trace-allocations', action='store_true',
                        help='Enable the test-only UCRT allocation-stack observer; changes measurement overhead')
    parser.add_argument('--memory-maps', action='store_true',
                        help='Save address-space and heap association metadata during retention control')
    args = parser.parse_args()
    if not 0 <= args.seconds <= 86400 or not 1 <= args.pairs <= 10000:
        parser.error('seconds must be 0..86400 and pairs must be 1..10000')
    if args.memory_maps and (not args.retention_control or args.trace_allocations):
        parser.error('memory-maps requires retention-control and must not be combined with trace-allocations')
    if args.gui_resources and (args.platform != 'windows' or not args.retention_control or args.trace_allocations):
        parser.error('gui-resources requires windows and retention-control, without trace-allocations')
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    report = dict(schema='FloraGPA persistent Qt soak runner 1', completed=False,
                  passed=False, minimum_seconds=args.seconds, minimum_pairs=args.pairs,
                  retention_control=args.retention_control,
                  trace_allocations=args.trace_allocations,
                  memory_maps=args.memory_maps,
                  workflows=args.workflows,
                  qt_platform=args.platform,
                  gui_resources=args.gui_resources,
                  gui_workflow_stages=args.gui_resources and args.workflows,
                  scope=f'Same-host {args.platform} Qt window, system-only PATH, serial production workers; not clean-machine or all-workflow certification')

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
        for name in ['tests/RecoveryUiTests.cpp', 'tests/RecoveryJournal.h', 'tests/RecoveryWorkflows.h',
                     'tests/HeapRetentionProbe.h', 'tests/ProcessMemorySnapshot.h', 'tools/validate_recovery_soak.py',
                     'tests/GuiResourceSnapshot.h', 'tests/GuiResourceTests.cpp',
                     'tools/test_recovery_soak.py', 'tools/validate_source_build.py']:
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
                   QT_QPA_PLATFORM=args.platform,
                   QTEST_FUNCTION_TIMEOUT=str((args.seconds + args.pairs * 180 + 600) * 1000),
                   FLORA_TEST_CAPTURE_DIR=str(args.captures.resolve()),
                   FLORA_RECOVERY_MIN_SECONDS=str(args.seconds),
                   FLORA_RECOVERY_ITERATIONS=str(args.pairs),
                   FLORA_RECOVERY_JOURNAL=str(root / 'journal.json'))
        if args.retention_control:
            env.update(FLORA_RECOVERY_HEAP='1', FLORA_RECOVERY_RETENTION_CONTROL='1')
        if args.trace_allocations:
            env['FLORA_HEAP_TRACE_DIR'] = str(root / 'allocation-stacks')
        if args.memory_maps:
            env['FLORA_MEMORY_MAP_DIR'] = str(root / 'memory-maps')
        if args.gui_resources:
            env['FLORA_GUI_RESOURCE_DIR'] = str(root / 'gui-resources')
        if args.workflows:
            env['FLORA_RECOVERY_WORKFLOWS'] = str(root / 'workflows')
        report['test_environment'] = {k: v for k, v in env.items() if k.startswith(('FLORA_', 'QT')) or k == 'PATH'}
        command = [str(executable), 'originalCaptureRecovery', '-o', str(root / 'qt-results.txt') + ',txt']
        report['command'] = command
        save()
        start = time.monotonic()
        with (root / 'process.log').open('wb') as log:
            code = run_process(command, runtime, env, log,
                               timeout=args.seconds + args.pairs * 180 + 900)
        report.update(exit_code=code, process_seconds=time.monotonic() - start)
        save()
        assert code == 0, 'Qt recovery test failed; inspect qt-results.txt and journal.json.progress'
        text = (root / 'qt-results.txt').read_text(encoding='utf-8')
        assert 'Totals: 3 passed, 0 failed, 0 skipped' in text, 'Missing complete Qt result'
        journal = json.loads((root / 'journal.json').read_text(encoding='utf-8'))
        assert journal['completed'] and journal['phase'] == 'complete'
        validate_platform(args.platform, journal)
        assert journal['workflows'] == args.workflows
        assert journal['gui_workflow_stages'] == (args.gui_resources and args.workflows)
        assert journal['elapsed_ms'] >= args.seconds * 1000
        rows = journal['observations']
        assert len(rows) == journal['completed_cycles'] and len(rows) >= args.pairs * 2 and len(rows) % 2 == 0
        for index, row in enumerate(rows):
            case = wanted[index % 2]
            assert row['capture'] == Path(case['path']).name and row['iteration'] == index // 2
            assert row['rgba_sha256'] == case['reference_rgba_sha256']
        if args.workflows:
            report['workflow_files'] = validate_workflows(root / 'workflows', rows)
        if args.gui_resources:
            report['gui_resource_files'] = validate_gui_resources(root / 'gui-resources', journal)
        if args.retention_control:
            controls = journal['retention_control']
            assert set(controls) == {'before', 'after_test_history_clear', 'after_log_clear',
                                    'after_pixmap_cache_clear'}
            assert all(sample['heap_walk_complete'] for sample in controls.values())
            assert all(row['ownership']['heap_walk_complete'] for row in rows)
            retained = root / 'journal.json.retained.json'
            saved = json.loads(retained.read_text(encoding='utf-8'))
            assert not saved['completed'] and saved['observations'] == rows
            report['retained_journal_sha256'] = digest(retained)
        if args.memory_maps:
            names = {'warmup','baseline','before_clear','after_history_clear','after_log_clear','after_pixmap_cache_clear'}
            assert set(journal['memory_maps']) == names
            report['memory_map_files'] = {}
            for name in sorted(names):
                path = root / 'memory-maps' / (name + '.json')
                snapshot = json.loads(path.read_text(encoding='utf-8'))
                sample = snapshot['summary']
                assert sample == journal['memory_maps'][name]
                assert sample['address_walk_complete'] and sample['heap_walk_complete']
                assert sum(a['private_committed'] for a in snapshot['allocations']) == sample['private_committed']
                assert sum(a['private_committed'] for a in snapshot['allocations'] if a['heap_busy_blocks']) == sample['private_committed_in_heap_allocations']
                assert sum(r['heap_busy_blocks'] for r in snapshot['regions']) + sample['unmapped_heap_blocks'] == sample['heap_busy_blocks']
                report['memory_map_files'][path.relative_to(root).as_posix()] = digest(path)
        if args.trace_allocations:
            expected_epochs = {2, args.pairs * 2}
            if len(rows) >= 6:
                expected_epochs.add(6)
            if args.retention_control:
                expected_epochs.update(range(len(rows) + 1, len(rows) + 5))
            report['allocation_stacks'] = {}
            for epoch in sorted(expected_epochs):
                path = root / 'allocation-stacks' / f'heap-{epoch}.json'
                snapshot = json.loads(path.read_text(encoding='utf-8'))
                assert snapshot['epoch'] == epoch and snapshot['dropped'] == 0
                report['allocation_stacks'][path.relative_to(root).as_posix()] = digest(path)
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
