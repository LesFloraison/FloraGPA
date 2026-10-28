"""Audit independent interface dispatch evidence without accepting GPU mismatches."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path
from validate_texture_precision import instructions, require


FIXTURES = ('array', 'static-array', 'single', 'single-u3', 'single-11_0', 'single-dynamic')
VENDORS = (0, 4318, 32902)


def validate(report, binary):
    fixture = report['fixture']
    single = fixture.startswith('single')
    dynamic = fixture in ('array', 'single-dynamic')
    require(fixture in FIXTURES and report['schema'] == 1
            and report['probe'] == 'original_dxbc_interfaces', 'Probe identity')
    require(report['vendor'] in (5140, 4318, 32902) and report['device'] > 0
            and report['luid_low'] | report['luid_high'], 'Actual adapter identity')
    require(report['warp'] is (report['vendor'] == 5140), 'Adapter selection')
    require(report['feature_level'] == (0xb000 if fixture == 'single-11_0' else 0xb100),
            'Explicit feature-level control')
    slots = 2 if not single or dynamic else 1
    require(report['slots'] == slots and report['seed'] == 0xdeadbeef, 'Binding and output seed')
    require(report['output_slot'] == (0 if single and fixture != 'single-u3' else 3), 'UAV slot control')
    require(all(d['severity'] > 1 for d in report['diagnostics']), 'No D3D11 validation errors')
    header, rows = instructions(binary.hex())
    require(header == 0x50050, 'Original CS 5.0 program')
    declarations = [r for r in rows if r[0] & 2047 == 146]
    require(len(declarations) == 1 and bool(declarations[0][0] & 0x800) is dynamic
            and declarations[0][3] >> 16 == slots, 'Original static/dynamic interface declaration')
    require(sum(r[0] & 2047 == 120 for r in rows) == (1 if single else 2), 'Interface call coverage')
    require([c['mode'] for c in report['cases']] == list(range(2 if single else 4)), 'Complete binding modes')
    comparisons = mismatches = 0
    for mode, case in enumerate(report['cases']):
        if single:
            expected = [i + (40 if mode else 20) for i in range(4)]
        else:
            a, b = ((20, 30), (10, 40), (40, 20), (20, 30))[mode]
            expected = [i * b + (i ^ a) if mode & 1 and fixture != 'static-array'
                        else i + a + i + b * 3 for i in range(4)]
        require(case['expected'] == expected and len(case['actual']) == 4, 'Independent authored arithmetic')
        require(len(case['bindings']) == slots, 'Every actual interface binding is recorded')
        for slot, binding in enumerate(case['bindings']):
            desc = binding['descriptor']
            require(len(desc) == 8 and desc[7] == (int(mode != 0) if single else int(mode >= 2)),
                    'Observed class creation mode')
            if desc[7]:
                require(binding['type'] == ('A' if single or slot == 0 else 'B') and not binding['name'],
                        'Created class type identity')
            else:
                require(binding['name'] == ('first' if single or slot == 0 else 'second')
                        and not binding['type'], 'Named class instance identity')
        comparisons += sum(a == b for a, b in zip(case['actual'], expected))
        mismatches += sum(a != b for a, b in zip(case['actual'], expected))
    require(report['passed'] is (mismatches == 0), 'A mismatch must never be reported as passing')
    return comparisons, mismatches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    manifest = json.loads((args.directory / 'matrix.json').read_text(encoding='utf-8-sig'))
    require(manifest['schema'] == 1 and manifest['complete'] is True, 'Complete research manifest')
    require([(r['vendor'], r['fixture']) for r in manifest['records']] ==
            list(itertools.product(VENDORS, FIXTURES)), 'Full independent control matrix')
    expected_files = set()
    for vendor, fixture in itertools.product(VENDORS, FIXTURES):
        name = f'{vendor}-{fixture}'
        expected_files.update(f'{name}/{suffix}' for suffix in ('shader.hlsl', 'shader.dxbc', 'shader.asm', 'result.json'))
        expected_files.update(f'{name}.{stream}.txt' for stream in ('stdout', 'stderr'))
    require(len(manifest['files']) == len(expected_files)
            and {entry['path'] for entry in manifest['files']} == expected_files, 'Complete evidence file manifest')
    for entry in manifest['files']:
        path = (args.directory / entry['path']).resolve()
        require(path.is_relative_to(args.directory.resolve()), 'Evidence path containment')
        require(hashlib.sha256(path.read_bytes()).hexdigest() == entry['sha256'], 'Evidence digest')
    compared = mismatched = corruptions = failed = 0
    for record in manifest['records']:
        directory = args.directory / record['name']
        require(record['name'] == f"{record['vendor']}-{record['fixture']}", 'Probe directory identity')
        report = json.loads((directory / 'result.json').read_text())
        binary = (directory / 'shader.dxbc').read_bytes()
        require(report['vendor'] == (record['vendor'] or 5140) and report['fixture'] == record['fixture'],
                'Requested and observed probe identity')
        matches, differences = validate(report, binary)
        compared += matches
        mismatched += differences
        failed += int(differences != 0)
        require(record['passed'] is report['passed'] and record['exit_code'] == int(differences != 0),
                'Probe process outcome')
        for key, value in (('passed', not report['passed']), ('slots', 253), ('seed', 0),
                           ('feature_level', 0), ('output_slot', 7)):
            changed = copy.deepcopy(report)
            changed[key] = value
            try:
                validate(changed, binary)
            except (ValueError, KeyError, IndexError):
                corruptions += 1
            else:
                raise ValueError(f'Accepted corrupted evidence: {key}')
    require(manifest['passed'] is (failed == 0), 'Matrix outcome retains every failure')
    print(json.dumps({'evidence_valid': True, 'acceptance_passed': failed == 0,
                      'probes': len(manifest['records']), 'failing_probes': failed,
                      'matching_words': compared, 'mismatching_words': mismatched,
                      'corruption_rejections': corruptions}, indent=2))
    return int(failed != 0)


if __name__ == '__main__':
    raise SystemExit(main())
