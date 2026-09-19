"""Development-only Python/native captured command-state comparison."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--captures', type=Path, required=True)
p.add_argument('--fixtures', type=Path, required=True)
p.add_argument('--qt-bin', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
args = p.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(args.reference.resolve()), str(args.reference.resolve().parent / 'tools')]
from frame import Frame
from api_commands import commands
from command_state import inspect, ContextState, command_context

# Reuse the original independent validator's fixture, including its KEEP and hazard cases.
spec = importlib.util.spec_from_file_location('reference_state_validation', args.reference.parent / 'tools/validate_command_state.py')
reference_validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference_validator)
reference_fixture = args.out / 'reference.gpa_frame'
reference_validator.fixture(reference_fixture)
env = dict(os.environ)
env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env['PATH']

def difference(actual, expected, path='result'):
    if actual == expected:
        return None
    if isinstance(actual, dict) and isinstance(expected, dict):
        if actual.keys() != expected.keys():
            return path, sorted(actual), sorted(expected)
        for k in expected:
            diff = difference(actual[k], expected[k], path + '.' + k)
            if diff:
                return diff
    if isinstance(actual, list) and isinstance(expected, list):
        if len(actual) != len(expected):
            return path + '.length', len(actual), len(expected)
        for i, (a, b) in enumerate(zip(actual, expected)):
            diff = difference(a, b, path + '[' + str(i) + ']')
            if diff:
                return diff
    return path, actual, expected

results = []
cases = [
    ('gf2', args.captures / 'GF2_Exilium_2026_03_03__00_19_35.gpa_frame', [79, 430]),
    ('bf1', args.captures / 'bf1_2026_01_21__16_53_05.gpa_frame', [4, 20434, 25572, 27696]),
    ('native', args.fixtures / 'state.gpa_frame', None),
    ('reference', reference_fixture, None),
]
for name, path, events in cases:
    with Frame(path) as frame:
        items = commands(frame)
        if events is None:
            events = [item['id'] for item in items]
        for event in events:
            for after in (False, True):
                key = f'{name}-{event}-' + ('after' if after else 'before')
                expected = inspect(frame, event, after, items)
                target = args.out / key
                command = [str(args.exe.resolve()), 'command-state', str(path.resolve()), '--event', str(event), '--out', str(target)]
                if not after:
                    command.append('--before')
                run = subprocess.run(command, env=env, capture_output=True, timeout=180)
                (args.out / (key + '.log')).write_bytes(run.stdout + run.stderr)
                assert run.returncode == 0, (key, run.stderr)
                actual = json.loads((target / 'command-state.json').read_text(encoding='utf-8'))
                diff = difference(actual, expected)
                if diff:
                    (args.out / (key + '-reference.json')).write_text(json.dumps(expected, indent=2), encoding='utf-8')
                assert diff is None, (key, diff)
                results.append(dict(case=key, passed=True, fields=len(expected['fields'])))
                print(key, 'PASS', flush=True)
        if name in ('gf2', 'bf1'):
            states = {}
            checked, differences = 0, []
            for item in items:
                context = command_context(frame, item)
                if context is None:
                    continue
                if context not in states:
                    states[context] = ContextState(frame, context)
                state = states[context]
                if item.get('draw'):
                    count, changes = state.anchor(item)
                    checked += count
                    differences.extend(dict(event=item['id'], **x) for x in changes)
                else:
                    state.apply(item)
            expected = dict(checked=checked, differences=differences, notes=[n for s in states.values() for n in s.notes])
            actual = json.loads((args.fixtures / (name + '-audit.json')).read_text(encoding='utf-8'))
            assert difference(actual, expected) is None, (name, difference(actual, expected))
            results.append(dict(case=name+'-all-snapshots', passed=True, compared_fields=checked))
            print(name, checked, 'snapshot fields PASS', flush=True)
(args.out / 'validation.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
