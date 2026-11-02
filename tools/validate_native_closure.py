"""Serial packaged GPA-scope checks with complete process/image event audits.

Use the same-source package. Original GPA differential probes are run separately.
Historical mesh/replay baselines are inputs, never substituted for new outputs.
A completed matrix can contain a recorded profile failure; that returns nonzero.
"""
import argparse
import hashlib
import json
from pathlib import Path
from audit_native_runtime import audit
from validate_gpa_event_history import compare


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', required=True, type=Path)
    parser.add_argument('--captures', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--original-results', required=True, type=Path)
    parser.add_argument('--resume', action='store_true', help='Reuse successful audits for the identical package')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=args.resume)
    package = args.package.resolve()
    cli = str(package / 'FloraGPA.Cli.exe')
    identity = {str(p.relative_to(package)): digest(p) for p in package.rglob('*') if p.is_file()}
    identity_path = output / 'package-identity.json'
    if args.resume:
        assert read(identity_path) == identity, 'Package changed; use a new validation directory'
    else:
        identity_path.write_text(json.dumps(identity, indent=2) + '\n', encoding='utf-8')
    completion_path = output / 'complete.json'
    if completion_path.exists():
        completion_path.unlink()
    rows = []
    def run(name, command, timeout=300, allow_profile_failure=False):
        row = {'name': name}
        rows.append(row)
        try:
            audit_dir = output / (name + '-audit')
            previous = sorted(output.glob(name + '-audit*/audit.json'), key=lambda p: p.stat().st_mtime)
            successful = [read(p) for p in previous if read(p).get('audit_passed')
                          and read(p).get('root_exit_code') in ((0, 1) if allow_profile_failure else (0,))
                          and read(p)['command'] == command]
            if args.resume and successful:
                result = successful[-1]
            else:
                if audit_dir.exists():
                    audit_dir = output / (name + '-audit-retry-' + str(len(previous)))
                if '--out' in command:
                    destination = Path(command[command.index('--out') + 1]).resolve()
                    assert destination.parent == output, 'Output must stay within validation root'
                    if destination.exists() and any(destination.iterdir()):
                        saved = output / (destination.name + '-failed-output-' + str(len(previous)))
                        assert not saved.exists()
                        destination.rename(saved)
                        row['preserved_output'] = str(saved)
                result = audit(command, audit_dir, timeout)
            row.update(audit_passed=result['audit_passed'], exit_code=result['root_exit_code'],
                       process_count=len(result['processes']), image_events=len(result['images']))
            if not result['audit_passed'] or result['root_exit_code'] not in ((0, 1) if allow_profile_failure else (0,)):
                raise RuntimeError('Workload or dependency audit failed: ' + name)
            if command[1] == 'analyze':
                result_dir = Path(command[command.index('--out') + 1])
                payload = read(result_dir / 'result.json')
                assert payload['ok'] and payload['backend'] == 'dx11-native/21'
                for relative, entry in payload['artifacts'].items():
                    artifact = (result_dir / relative).resolve()
                    assert artifact.is_relative_to(result_dir)
                    assert artifact.stat().st_size == entry['bytes']
                    assert digest(artifact) == entry['sha256']
                row['verified_artifacts'] = len(payload['artifacts'])
            row['execution_passed'] = result['root_exit_code'] == 0
        except Exception as error:
            row.update(execution_passed=False, error=str(error))
            raise
        finally:
            (output / 'validation.json').write_text(json.dumps(rows, indent=2) + '\n', encoding='utf-8')
        print(name, 'PASS' if row['execution_passed'] else 'PROFILE VALIDATION FAILED (retained)', flush=True)
        return row
    cases = [('gf2', 'GF2_Exilium_2026_03_03__00_19_35.gpa_frame', 113),
             ('bf1', 'bf1_2026_01_21__16_53_05.gpa_frame', 876),
             ('helldivers', 'helldivers2_2026_04_02__18_02_58.gpa_frame', 200)]
    for vendor in (4318, 32902, 0):
        device = ['--gpu-vendor', str(vendor)] if vendor else ['--warp']
        for name, filename, event in cases:
            frame = str((args.captures / filename).resolve())
            prefix = f'{vendor}-{name}'
            target = output / (prefix + '-replay')
            row = run(prefix + '-replay', [cli, 'replay', frame, '--out', str(target), *device])
            report = read(target / 'report.json')
            assert report['completed'] and not report['reference_pixels_used']
            row['rgba_sha256'] = digest(target / 'frame.rgba')
            assert row['rgba_sha256'] == report['rgba_sha256']
            if vendor == 4318 and name != 'helldivers':
                baseline = repo / f'artifacts/native-gpa-package-{"hd2" if name == "helldivers" else name}-replay/report.json'
                assert row['rgba_sha256'] == read(baseline)['rgba_sha256'], 'Same-device replay regression'
                row['same_device_baseline'] = str(baseline.relative_to(repo))
            if name == 'helldivers':
                policy = next(x for x in read(repo / 'docs/capture-corpus.json')['cases'] if x['path'] == filename)
                assert policy['comparison_policy'] == 'known_variable'
                row['image_policy'] = 'known_variable; completion and bounded controls, not full-frame hash identity'
                if vendor == 4318:
                    old_cli = str(repo / 'out/FloraGPA-native-20261102/FloraGPA.Cli.exe')
                    controls = {}
                    variants = [('before', ['--event', '18401', '--before']),
                                ('no-draws', ['--suppress-draws']),
                                *[(f'disabled-{i}', ['--disable', '18401']) for i in range(3)]]
                    for label, flags in variants:
                        control = output / (prefix + '-' + label)
                        run(prefix + '-' + label, [cli, 'replay', frame, '--out', str(control), *device, *flags])
                        observed = read(control / 'report.json')
                        assert observed['completed'] and not observed['reference_pixels_used']
                        controls[label] = observed['rgba_sha256']
                        if label.startswith('disabled-'):
                            assert sum(v for k, v in observed['counts'].items() if k.startswith('Dispatch')) == 129
                    assert len({controls[f'disabled-{i}'] for i in range(3)}) == 1
                    assert controls['no-draws'] != row['rgba_sha256']
                    for label, flags in [('before', ['--event', '18401', '--before']),
                                         ('disabled-0', ['--disable', '18401'])]:
                        control = output / (prefix + '-prior-' + label)
                        run(prefix + '-prior-' + label, [old_cli, 'replay', frame, '--out', str(control), *device, *flags])
                        assert read(control / 'report.json')['rgba_sha256'] == controls[label], 'Stable Helldivers boundary changed'
                    row['controls'] = controls
            # Intel Helldivers replays exceed the interactive 180-second default.
            analysis_budget = ['--timeout', '600'] if vendor == 32902 and name == 'helldivers' else []
            analysis_deadline = 660 if analysis_budget else 300
            resource = str(report['resource'])
            run(prefix + '-texture', [cli, 'analyze', frame, 'texture', '--resource', resource,
                                      '--gpa-event', str(report['output_selection']['event']), '--out', str(output / (prefix + '-texture')), *device, *analysis_budget], analysis_deadline)
            assert read(output / (prefix + '-texture/result.json'))['ok']
            mesh = output / (prefix + '-mesh')
            row = run(prefix + '-mesh', [cli, 'analyze', frame, 'mesh', '--gpa-event', str(event),
                                         '--stage', 'final', '--instance', '0', '--out', str(mesh), *device, *analysis_budget], analysis_deadline)
            assert read(mesh / 'result.json')['ok']
            baseline = repo / f'artifacts/dx11-native/mesh-real-verified/{vendor}-{name}-{event}-0/final/vertices.bin'
            assert digest(mesh / 'vertices.bin') == digest(baseline), 'Same-device mesh regression'
            row['vertices_sha256'] = digest(mesh / 'vertices.bin')
            row['same_device_baseline'] = str(baseline.relative_to(repo))
            counters = output / (prefix + '-counters')
            run(prefix + '-counters', [cli, 'analyze', frame, 'counters', '--out', str(counters), *device, *analysis_budget], analysis_deadline)
            assert read(counters / 'result.json')['ok']
            if vendor == 32902:
                metrics = output / (prefix + '-intel-metrics')
                metric_row = run(prefix + '-intel-metrics', [cli, 'metric-profile', frame, '--set', 'RenderBasic',
                                                '--event', str(event), '--out', str(metrics)], 900 if name == 'helldivers' else 300, allow_profile_failure=name == 'helldivers')
                if metric_row['execution_passed']:
                    assert read(metrics / 'profile.json')['records'], 'Missing Intel metric records'
                    metric_row['qualification'] = 'collection only; numerical hardware qualification remains failed'
                else:
                    validation = read(metrics / 'validation.json')
                    assert validation['event_mapping_valid'] and validation['frame_matches'] and validation['experiment_matches']
                    assert not validation['image_matches'] and not (metrics / 'profile.json').exists()
                    metric_row['status'] = 'validation_failed_no_profile'
                    metric_row['qualification'] = 'Not passed: Helldivers full image differs between baseline and measurement; capture is known_variable'
                    metric_row['validation'] = str((metrics / 'validation.json').relative_to(repo))
    fixture = str(repo / 'artifacts/dx11-native/pixel-history-followup/fixture-nvidia.gpa_frame')
    for name, vendor in [('nvidia', 4318), ('intel', 32902)]:
        target = output / ('history-' + name)
        row = run('history-' + name, [cli, 'analyze', fixture, 'history', '--history-scope', 'gpa',
                                      '--resource', '5', '--x', '4', '--y', '4', '--gpu-vendor', str(vendor),
                                      '--out', str(target)])
        row['differential'] = compare(read(target / 'result.json'),
                                      read(args.original_results / ('original-' + name) / 'history.json'))
    run('gui-gf2', [str(package / 'FloraGPA.exe'), '-platform', 'offscreen', str((args.captures / cases[0][1]).resolve()),
                    '--screenshot', str(output / 'gui-gf2.png'), '--size', '1600x950'])
    run('intel-catalog', [cli, 'metric-catalog', '--out', str(output / 'intel-catalog')])
    (output / 'validation.json').write_text(json.dumps(rows, indent=2) + '\n', encoding='utf-8')
    completion_path.write_text(json.dumps({'completed': True, 'workloads': len(rows),
        'execution_passed': sum(r['execution_passed'] for r in rows),
        'profile_validation_failed': sum(r.get('status') == 'validation_failed_no_profile' for r in rows),
        'all_dependency_audits_passed': all(r['audit_passed'] for r in rows)}, indent=2) + '\n', encoding='utf-8')
    return 0 if all(r['execution_passed'] for r in rows) else 1


if __name__ == '__main__':
    raise SystemExit(main())
