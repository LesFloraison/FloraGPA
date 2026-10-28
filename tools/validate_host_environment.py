"""Validate colocated GPA research evidence without inferring a debugger engine."""
import argparse
import copy
import itertools
import json
from pathlib import Path
import stat
import struct

from probe_original_debugger_host import PLAYER_SHA256, digest
from summarize_debugger_gpu import validate_dispatch
from validate_module_route_research import exports, require


def path(value):
    return Path(value.removeprefix('//?/').removeprefix('\\\\?\\')).resolve()


def validate_environment(report, colocated, sandbox, endpoints):
    require(report['completed'] and not report['engine_recovered'] and not report['shader_executed'],
            'Incomplete or overstated environment scope')
    require(not report['parser_preloaded'] and not report['parser_loaded_by_player_import'] and
            report['parser_loaded'] == colocated and report['returned_self'], 'Relative load outcome')
    require(report['released'] == colocated and not report['second_release'] and
            not report['parser_loaded_after_release'], 'Parser reference lifetime')
    executable = path(report['executable'])
    require(executable == path(report['python_executable']) and
            (executable.parent == sandbox) == colocated, 'Actual executable identity')
    require(report['directories'] == [
        dict(kind='executable', getter_rva='0xcea40', value=str(executable.parent) + '\\'),
        dict(kind='module', getter_rva='0xce640', value=str(sandbox) + '\\')], 'Original directory getters')
    expected = bytearray(b'\xa5' * 136)
    require(bytes.fromhex(report['initial']) == expected, 'Guard seed')
    actual = bytes.fromhex(report['constructed'])
    require(len(actual) == 136, 'Wrapper allocation size')
    handle, = struct.unpack_from('<Q', actual, 40)
    require(bool(handle) == colocated, 'Wrapper handle')
    struct.pack_into('<QQ', expected, 32, 0x123456789abcdef0, handle)
    expected[48:64] = b'ShaderParse.dll\0'
    struct.pack_into('<QQ', expected, 64, 15, 15)
    names = ('Assemble', 'GetAsmParseError', 'Release')
    for index, name in enumerate(names):
        struct.pack_into('<Q', expected, 80 + 8 * index, handle + endpoints[name] if colocated else 0)
    require(actual == expected, 'Original constructor bytes or endpoint identity')
    struct.pack_into('<Q', expected, 40, 0)
    expected[48] = 0
    struct.pack_into('<Q', expected, 64, 0)
    require(bytes.fromhex(report['cleaned']) == expected, 'Original cleanup bytes')
    require(report['endpoints'] == ([dict(name=n, rva=hex(endpoints[n])) for n in names] if colocated else []),
            'Parser export mapping')
    if colocated:
        require(path(report['parser_path']) == sandbox / 'ShaderParse.dll', 'Loaded parser path')
    for key, present in [('modules_before', False), ('modules_constructed', colocated), ('modules_after', False)]:
        parser_paths = [path(p) for p in report[key] if path(p).name.lower() == 'shaderparse.dll']
        require(parser_paths == ([sandbox / 'ShaderParse.dll'] if present else []), 'Parser module snapshots')


def validate_lifecycle(watch, dispatch, sandbox, copied_names, colocated, original, vendor):
    validate_dispatch(dispatch)
    settings = {'single-threaded': 'true'}
    if not original:
        settings.update({'no-gtquerymanager': 'true', 'no-first-playbacks': 'true'})
    require(dispatch['requested_vendor'] == vendor and dispatch['settings'] == settings, 'Requested matrix case')
    require(watch['completed'] and watch['exit_code'] == 0 and not watch['engine_recovered'] and
            not watch['shader_debug_parity'] and watch['player_sha256'] == PLAYER_SHA256 and
            watch['constructor_hits'] == watch['destructor_hits'] == 1 and watch['request_hits'] == 2 and
            0 < watch['elapsed_ms'] < watch['timeout_ms'] == 120000, 'Hardware-watch lifetime coverage')
    writes = [e for e in watch['events'] if e['hits'] & 4]
    require(len(writes) == watch['field_writes'] == 2 and watch['nonzero_live_field_writes'] == 0 and
            writes[0]['lifecycle_phase'] == 'constructing' and writes[0]['written_engine'] == '0x0' and
            writes[1]['lifecycle_phase'] == 'destroying', 'Observed field-write phases')
    requests = [e for e in watch['events'] if e['hits'] & 8]
    require(len(requests) == 2 and all(e['request_engine'] == '0x0' and
            e['instruction_module']['rva'] == '0x902b0' for e in requests), 'Observed request entry')
    original_modules = [m for m in watch['modules'] if path(m['path']).name.lower() in copied_names]
    require({path(m['path']).name.lower() for m in original_modules} ==
            {'dx11_player.dll', 'd3dcompiler_47.dll'} and
            all(path(m['path']).parent == sandbox for m in original_modules), 'GPA module load observations')
    for snapshot in dispatch['snapshots']:
        # The watcher's LOAD_DLL events exclude CREATE_PROCESS's executable.
        # Boundary module snapshots independently include the actual host EXE.
        hosts = [path(m) for m in snapshot['modules'] if path(m).name.lower() ==
                 ('floraoriginalresearchhost.exe' if colocated else 'python.exe')]
        require(len(hosts) == 1 and (hosts[0].parent == sandbox) == colocated, 'Actual lifecycle executable')
        require(snapshot['engine'] == '0x0', 'Non-null boundary observation')
        if snapshot['host'] == '0x0' or snapshot['label'] == 'closed':
            continue
        graph = snapshot['host_graph']
        require(graph['host'] == snapshot['host'] and graph['engine'] == '0x0' and len(graph['aliases']) == 3 and
                all(a['matches_host'] and a['pointer'] == graph['host'] for a in graph['aliases'].values()),
                'Host owner aliases')
        api, query = graph['api'], graph['query']
        require(api['vtable_rva'] == '0x17ae40' and api['owner_matches_host'] and api['owner'] == graph['host'],
                'API manager identity')
        require(query['vtable_rva'] in ('0x180538', '0x17d668'), 'Known query manager identity')
        if query['vtable_rva'] == '0x180538':
            require(query['owner_matches_host'] and query['owner'] == graph['host'], 'Query owner')
        else:
            require(query['api_matches_host_manager'] and query['api'] == api['pointer'], 'Query API alias')
    return [m['path'] for m in original_modules]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--recovery', type=Path, action='append', default=[],
                        help='Supplemental manifests after the retained Helldivers timeout')
    args = parser.parse_args()
    root = args.root.resolve()
    read = lambda p: json.loads(p.read_text(encoding='utf-8-sig'))
    identity = lambda p: dict(path=p.as_posix(), bytes=p.stat().st_size, sha256=digest(p))
    ledger = dict(schema='FloraGPA host environment evidence 1', completed=False,
                  engine_recovered=False, shader_debug_parity=False, renderdoc_removed=False,
                  files=[], environments=[], calibrations=[], lifecycles=[], negative_controls=[],
                  retained_failures=[])

    def rejected(name, check):
        try:
            check()
        except ValueError as error:
            ledger['negative_controls'].append(dict(name=name, reason=str(error)))
        else:
            raise ValueError('Corrupt evidence accepted: ' + name)

    for capture, event in [('gf2', 113), ('bf1', 876), ('helldivers', 200)]:
        directory = root / capture
        manifest_path = directory / 'runs.json'
        manifest = read(manifest_path)
        require(manifest['files_unchanged'] and manifest['capture_unchanged'] and
                not manifest['engine_recovered'] and not manifest['shader_debug_parity'], 'Runner completion')
        ledger['files'].append(identity(manifest_path))
        runs = [(directory, r) for r in manifest['runs']]
        if not manifest['completed']:
            require(capture == 'helldivers' and args.recovery, 'Unexpected incomplete matrix')
            for recovery_path in args.recovery:
                recovery = read(recovery_path)
                require(recovery['original_manifest']['sha256'] == digest(manifest_path), 'Recovery provenance')
                for source in recovery.get('sources', []):
                    require(digest(Path(source['path'])) == source['sha256'], 'Recovery source changed')
                ledger['files'].append(identity(recovery_path.resolve()))
                runs += [(recovery_path.resolve().parent, r) for r in recovery['runs']]
        for item in manifest['sources'] + [manifest['capture'], manifest['launcher_copy']]:
            require(digest(Path(item['path'])) == item['sha256'], 'Input identity changed')
        require(len(manifest['copied_files']) == 72, 'Pinned root DLL inventory')
        names = set()
        for row in manifest['copied_files']:
            require(row['original']['sha256'] == row['copy']['sha256'], 'Copied identity')
            for side in ('original', 'copy'):
                require(digest(Path(row[side]['path'])) == row[side]['sha256'], 'Research DLL changed')
            require(Path(row['copy']['path']).stat().st_file_attributes & stat.FILE_ATTRIBUTE_READONLY,
                    'Copied DLL is not read-only')
            names.add(Path(row['copy']['path']).name.lower())
        require(len(names) == 72, 'Duplicate copied module')
        sandbox = directory / 'modules'
        endpoints = exports(sandbox / 'ShaderParse.dll')
        combinations = list(itertools.product(('python', 'colocated'), (False, True), (4318, 32902)))
        expected = {'usage': 2, 'missing-runtime': 2, 'wrong-runtime': 3, 'python': 0, 'colocated': 0}
        expected.update({f'calibration-{i}': 0 for i in range(3)})
        expected.update({label + '-' + ('original' if original else 'controlled') + '-' + str(vendor): 0
                         for label, original, vendor in combinations})
        require({r['name'] for _, r in runs} == set(expected), 'All matrix cases attempted')
        run_directories = {}
        for run_directory, row in runs:
            ledger['files'].append(identity(run_directory / (row['name'] + '.log')))
            require(not row['timed_out'] and row['expected_exit'] == expected[row['name']], 'Outer child deadline')
            if row['exit_code'] != row['expected_exit']:
                require(capture == 'helldivers' and row['name'] == 'colocated-controlled-32902' and
                        row['exit_code'] == 1, 'Retained failure identity')
                failed_directory = run_directory / row['name']
                failed_watch = read(failed_directory / 'watch.json')
                failed_dispatch = read(failed_directory / 'dispatch.json')
                require(not failed_watch['completed'] and failed_watch['error'] == 'Hardware watch timed out' and
                        failed_watch['player_sha256'] == PLAYER_SHA256 and
                        failed_watch['timeout_ms'] == 120000 and 120000 <= failed_watch['elapsed_ms'] < 130000,
                        'Bounded original timeout')
                require(not failed_dispatch['completed'] and not failed_dispatch['errors'] and
                        not failed_dispatch['observer_errors'] and
                        failed_dispatch['player_sha256'] == PLAYER_SHA256 and
                        all(s['engine'] == '0x0' for s in failed_dispatch['snapshots']) and
                        failed_dispatch['selected_gpu']['vendor'] == 32902 and failed_dispatch['event'] == event and
                        failed_dispatch['capture_sha256'] == manifest['capture']['sha256'],
                        'Incomplete lifecycle identity')
                labels = [s['label'] for s in failed_dispatch['snapshots']]
                observed_requests = [e for e in failed_watch['events'] if e['hits'] & 8]
                actual = failed_dispatch.get('replay_adapter')
                if actual:
                    selected = failed_dispatch['selected_gpu']
                    require(actual['vendor'] == selected['vendor'] and actual['device'] == selected['device'] and
                            actual['luid'] == (selected['gpu_id'][1] | selected['gpu_id'][2] << 32),
                            'Partial replay adapter identity')
                    require(labels == ['loaded', 'opened', 'replayed', 'after_control_request', 'after_observed_request'] and
                            failed_dispatch['playbacks'] == [dict(label='before_requests', status=0, error='0x0')] and
                            len(failed_dispatch['requests']) == 2 and len(observed_requests) == 2 and
                            all(r['status'] == 0 and r['error'] == '0x0' and r['observer_restored'] and
                                not r['result_callbacks'] for r in failed_dispatch['requests']) and
                            all(e['request_engine'] == '0x0' for e in observed_requests), 'Post-request timeout boundary')
                else:
                    require(labels == ['loaded', 'opened'] and not failed_dispatch['playbacks'] and
                            not failed_dispatch['requests'] and not observed_requests, 'Pre-request timeout boundary')
                require([e['hits'] for e in failed_watch['events']] == [2, 4, 1] + [8] * len(observed_requests) and
                        failed_watch['events'][1]['written_engine'] == '0x0', 'Partial field observations')
                ledger['retained_failures'].append(dict(capture=capture, name=row['name'],
                    report=identity(failed_directory / 'watch.json'), elapsed_ms=failed_watch['elapsed_ms'],
                    last_boundary=labels[-1], actual_replay_adapter_verified=bool(actual),
                    observed_null_requests=len(observed_requests), complete_lifecycle_inference=False))
                ledger['files'] += [identity(failed_directory / 'watch.json'), identity(failed_directory / 'dispatch.json')]
            else:
                require(row['name'] not in run_directories, 'Duplicate completed case')
                run_directories[row['name']] = run_directory
        for label in ('python', 'colocated'):
            report_path = directory / (label + '.json')
            report = read(report_path)
            validate_environment(report, label == 'colocated', sandbox, endpoints)
            ledger['files'].append(identity(report_path))
            ledger['environments'].append(dict(capture=capture, host=label, parser_loaded=report['parser_loaded'],
                                               directories=report['directories']))
            for key in ('completed', 'parser_preloaded', 'parser_loaded', 'parser_loaded_after_release',
                        'returned_self', 'released', 'second_release', 'engine_recovered'):
                changed = copy.deepcopy(report)
                changed[key] = not changed[key]
                rejected(capture + '/' + label + '/' + key,
                         lambda: validate_environment(changed, label == 'colocated', sandbox, endpoints))
            for key, offset in [('constructed', 0), ('constructed', 80), ('cleaned', 64)]:
                changed = copy.deepcopy(report)
                raw = bytearray.fromhex(changed[key]); raw[offset] ^= 1
                changed[key] = raw.hex()
                rejected(f'{capture}/{label}/{key}/{offset}',
                         lambda: validate_environment(changed, label == 'colocated', sandbox, endpoints))
        for index in range(3):
            report_path = directory / f'calibration-{index}.json'
            watch = read(report_path)
            writes = [e for e in watch['events'] if e['hits'] & 4]
            require(watch['completed'] and watch['self_test'] and watch['exit_code'] == 0 and
                    len(writes) == watch['field_writes'] == 36 and watch['nonzero_live_field_writes'] > 0 and
                    len({e['thread_id'] for e in writes}) == 3, 'Transient-write calibration')
            ledger['files'].append(identity(report_path))
            ledger['calibrations'].append(dict(capture=capture, index=index, field_writes=len(writes)))
        for label, original, vendor in combinations:
            name = label + '-' + ('original' if original else 'controlled') + '-' + str(vendor)
            if name not in run_directories:
                # Failed attempts are retained above and never enter the complete-lifecycle validator.
                continue
            run = run_directories[name] / name
            watch, dispatch = read(run / 'watch.json'), read(run / 'dispatch.json')
            gpa_modules = validate_lifecycle(watch, dispatch, sandbox, names, label == 'colocated', original, vendor)
            require(dispatch['event'] == event and dispatch['capture_sha256'] == manifest['capture']['sha256'],
                    'Capture/event identity')
            ledger['files'] += [identity(run / 'watch.json'), identity(run / 'dispatch.json')]
            ledger['lifecycles'].append(dict(capture=capture, name=name, event=event,
                                            elapsed_ms=watch['elapsed_ms'], adapter=dispatch['replay_adapter'],
                                            gpa_modules=gpa_modules, nonzero_live_field_writes=0))
            for key in ('request_hits', 'field_writes', 'nonzero_live_field_writes'):
                changed = copy.deepcopy(watch); changed[key] += 1
                rejected(capture + '/' + name + '/' + key, lambda: validate_lifecycle(
                    changed, dispatch, sandbox, names, label == 'colocated', original, vendor))
            changed_dispatch = copy.deepcopy(dispatch)
            changed_dispatch['replay_adapter']['luid'] ^= 1
            rejected(capture + '/' + name + '/adapter', lambda: validate_lifecycle(
                watch, changed_dispatch, sandbox, names, label == 'colocated', original, vendor))
    ledger['files'] += [identity(Path(__file__).resolve()),
                        identity(Path(__file__).with_name('summarize_debugger_gpu.py').resolve()),
                        identity(Path(__file__).with_name('validate_module_route_research.py').resolve())]
    ledger['completed'] = True
    ledger['every_attempt_completed'] = not ledger['retained_failures']
    ledger['complete_lifecycle_matrix'] = len(ledger['lifecycles']) == 24
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(ledger, stream, indent=2); stream.write('\n')
    print(f"Validated {len(ledger['environments'])} environments, {len(ledger['calibrations'])} calibrations, "
          f"{len(ledger['lifecycles'])} complete lifecycles, {len(ledger['retained_failures'])} retained failures "
          f"and {len(ledger['negative_controls'])} corruptions")


if __name__ == '__main__':
    main()
