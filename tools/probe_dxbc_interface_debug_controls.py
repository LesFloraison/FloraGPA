"""Run paired original interface shaders with and without the D3D11 debug layer."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import subprocess
from validate_texture_precision import require


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    require(not args.out.exists(), 'Use a fresh debug control directory')
    probe = args.probe.resolve()
    before = digest(probe)
    args.out.mkdir(parents=True)
    records = []
    sources, reference_inputs = {}, {}
    paired_words = failed = 0
    for vendor, fixture in itertools.product((4318, 32902, 0), ('single', 'wide-array', 'multi-array', 'wide-multi-array')):
        pair = []
        shaders = []
        for debug in (True, False):
            name = f'{vendor}-{fixture}-' + ('debug' if debug else 'plain')
            directory = args.out / name
            command = [str(probe), str(directory.resolve()), str(vendor), fixture]
            provided = fixture in sources
            if provided:
                command.extend(['debug' if debug else 'no-debug', str(sources[fixture].resolve())])
            process = subprocess.run(command, capture_output=True, timeout=30,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            (args.out / f'{name}.stdout.txt').write_bytes(process.stdout)
            (args.out / f'{name}.stderr.txt').write_bytes(process.stderr)
            report = json.loads((directory / 'result.json').read_text())
            require(report['schema'] == 1 and report['probe'] == 'original_dxbc_interfaces'
                    and report['fixture'] == fixture and report['vendor'] == (vendor or 5140)
                    and report['warp'] is (vendor == 0),
                    'Observed original control identity')
            require(report['requested_debug_layer'] is debug and report['creation_flags'] == (2 if debug else 0)
                    and report['validation_status'] == (0 if debug else 0x80004002)
                    and report['diagnostics'] == ([] if debug else None), 'Actual debug layer control')
            require(report['bytecode_origin'] == ('provided_original' if provided else 'compiled_fixture'),
                    'Reuse the exact original compiler output for the paired device')
            binary = directory / 'shader.dxbc'
            if not provided:
                sources[fixture] = binary
            require(digest(binary) == digest(sources[fixture]), 'Every adapter must use the same original DXBC')
            observed = {k: report[k] for k in ('feature_level', 'slots', 'seed', 'output_slot',
                                              'selected_base', 'extra_base', 'constant_words')}
            observed['cases'] = [{k: case[k] for k in ('mode', 'bindings', 'expected')} for case in report['cases']]
            require(observed == reference_inputs.setdefault(fixture, observed), 'Identical inputs across all adapters')
            require(report['passed'] is all(c['actual'] == c['expected'] for c in report['cases'])
                    and process.returncode == int(not report['passed']), 'Honest process result')
            failed += int(not report['passed'])
            pair.append(report)
            shaders.append(digest(binary))
            records.append({'directory': name, 'passed': report['passed'], 'exit_code': process.returncode})
        require(shaders[0] == shaders[1], 'The control must use identical original DXBC')
        for field in ('device', 'luid_low', 'luid_high', 'feature_level', 'slots', 'seed', 'output_slot',
                      'selected_base', 'extra_base', 'constant_words', 'cases'):
            require(pair[0][field] == pair[1][field], f'Debug control differs in {field}')
        paired_words += sum(len(case['actual']) for case in pair[0]['cases'])
    require(digest(probe) == before, 'Probe changed during the control matrix')
    files = [{'path': path.relative_to(args.out).as_posix(), 'sha256': digest(path)}
             for path in sorted(args.out.rglob('*')) if path.is_file()]
    summary = {'schema': 1, 'evidence_valid': True, 'probe_sha256': before, 'probes': len(records),
               'shared_shader_sha256': {fixture: digest(path) for fixture, path in sources.items()},
               'failed_probes': failed, 'identical_debug_plain_words': paired_words,
               'acceptance_passed': failed == 0, 'records': records, 'files': files}
    (args.out / 'matrix.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in summary.items() if k not in ('records', 'files')}, indent=2))
    return int(failed != 0)


if __name__ == '__main__':
    raise SystemExit(main())
