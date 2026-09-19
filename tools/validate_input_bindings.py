"""Development-only comparison of C++ binding evidence with Python native getters."""
import argparse
import json
from pathlib import Path
import sys

p = argparse.ArgumentParser()
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--fixtures', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--captures', type=Path)
args = p.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
sys.path.insert(0, str(args.reference.resolve()))
from frame import Frame
from dx11 import Device
from engine import Engine
from replay_pipeline import inspect

checks = []

def record(name, passed, **details):
    checks.append(dict(case=name, passed=passed, **details))
    (args.out / 'validation.json').write_text(json.dumps(checks, indent=2) + '\n')
    if not passed:
        raise AssertionError(checks[-1])

for driver in ('hardware', 'warp'):
    for name in ('inputs', 'gaps'):
        fixture = args.fixtures / f'{name}-{driver}.gpa_frame'
        rows = json.loads(fixture.with_suffix('.json').read_text())
        with Frame(fixture) as frame:
            device = Device(driver)
            try:
                engine = Engine(frame, device)
                for row in rows:
                    key = f'{name}-{driver}-{row["event"]}-{"before" if row["before"] else "after"}'
                    if row.get('error'):
                        error = None
                        try:
                            inspect(engine, row['event'], after=not row['before'])
                        except (ValueError, RuntimeError) as caught:
                            error = str(caught)
                        record(key, error is not None, error=error)
                    else:
                        result = inspect(engine, row['event'], after=not row['before'])
                        expected = {field['field']: field['value'] for field in result['fields']}
                        differences = {field: dict(native=value, python=expected.get(field))
                                       for field, value in row['values'].items()
                                       if value != expected.get(field)}
                        record(key, not differences, fields=len(row['values']), differences=differences)
            finally:
                device.close()
        print(name, driver, 'PASS', flush=True)

for fixture in sorted(args.fixtures.glob('invalid-*.gpa_frame')) + [args.fixtures / 'deferred.gpa_frame']:
    with Frame(fixture) as frame:
        device = Device('warp')
        try:
            error = None
            try:
                Engine(frame, device).replay(readback=False)
            except (ValueError, RuntimeError, KeyError) as caught:
                error = str(caught)
            record(fixture.stem, error is not None, error=error)
        finally:
            device.close()
if args.captures:
    for row in json.loads((args.fixtures / 'real.json').read_text()):
        key = f'{row["capture"]}-{row["event"]}-{"before" if row["before"] else "after"}'
        with Frame(args.captures / row['capture']) as frame:
            device = Device('hardware')
            try:
                engine = Engine(frame, device)
                if row.get('error'):
                    error = None
                    try:
                        inspect(engine, row['event'], after=not row['before'])
                    except (ValueError, RuntimeError) as caught:
                        error = str(caught)
                    record(key, error is not None, error=error)
                else:
                    result = inspect(engine, row['event'], after=not row['before'])
                    expected = {field['field']: field['value'] for field in result['fields']}
                    differences = {field: dict(native=value, python=expected.get(field))
                                   for field, value in row['values'].items()
                                   if value != expected.get(field)}
                    record(key, not differences, fields=len(row['values']), differences=differences)
            finally:
                device.close()
        print(key, 'PASS', flush=True)
print(len(checks), 'comparisons PASS', flush=True)
