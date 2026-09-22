"""Compare native MD descriptors, validation, catalog and saved reports with Python."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--bridge', type=Path, required=True)
    parser.add_argument('--qt-bin', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--isolated-env', action='store_true')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.reference / 'standalone'))
    from metric_kinds import descriptor_kind
    from metrics_discovery import Metrics, validate_result
    from devices import create_vendor_device, create_device
    jobs, expected, labels = [], [], []

    def add(label, job, reference):
        jobs.append(job)
        labels.append(label)
        try:
            expected.append(reference())
        except (ValueError, RuntimeError) as error:
            expected.append({'error': str(error)})

    for kind in range(8):
        for unit in ('percent', 'Percent', 'ns', '', 'MHz'):
            for symbol in ('GpuTime', 'VulkanGpuTime', 'D3D12GpuTime', 'AvgGpuCoreFrequencyMHz', 'Other'):
                add(f'kind {kind} {unit} {symbol}', dict(op='kind', type=kind, unit=unit, symbol=symbol),
                    lambda: descriptor_kind(kind, unit, symbol))
    for kind in (-1, 8, True, 1.0, None, '1'):
        add('invalid kind '+repr(kind), dict(op='kind', type=kind, unit='', symbol=''), lambda: descriptor_kind(kind, '', ''))
    for unit, symbol in ((None, ''), ('', 42)):
        add('invalid name/unit', dict(op='kind', type=0, unit=unit, symbol=symbol), lambda: descriptor_kind(0, unit, symbol))
    for driver in ('hardware', 'warp'):
        dev = create_device(driver)
        try:
            add('device '+driver, dict(op='device', warp=driver == 'warp'), lambda: dev.execution_info)
        finally:
            dev.close()
    device = create_vendor_device(0x8086)
    try:
        with Metrics(device, args.bridge.resolve()) as md:
            add('complete annotated catalog', dict(op='catalog'), lambda: md.catalog)
            for name in ('gf2-final', 'gf2-all-sets', 'gf2-edited-final', 'bf1-final'):
                folder = args.reference / 'output/metrics-discovery' / name
                profile = json.loads((folder / 'profile.json').read_text(encoding='utf-8'))
                for index, row in enumerate(profile['records']):
                    raw_path = folder / row['raw_report']
                    raw = raw_path.read_bytes()
                    assert len(raw) == row['raw_size']
                    assert hashlib.sha256(raw).hexdigest() == row['raw_sha256']
                    if md.selected is None or md.selected['name'] != row['set']:
                        md.select(row['set'])
                    decoded, same_raw = md.decode(raw)
                    assert same_raw == raw and decoded['values'] == row['values'] + row['information']
                    assert decoded['available'] == row['available'] and decoded['unavailable_reasons'] == row['unavailable_reasons']
                    add(f'{name}/{index}', dict(op='decode', set=row['set'], path=str(raw_path.resolve())),
                        lambda: dict(result=decoded, raw_hex=raw.hex()))
            metadata = copy.deepcopy(md.selected)
            baseline = copy.deepcopy(decoded)

            def validate(label, value, size=len(raw)):
                add(label, dict(op='validate', metadata=metadata, result=value, size=size),
                    lambda: validate_result(metadata, copy.deepcopy(value), bytes(size)))

            validate('valid report', baseline)
            validate('raw report size', baseline, len(raw)-1)
            for count in (0, 2, True, 1.0, None):
                value = copy.deepcopy(baseline); value['reports'] = count
                validate('report count '+repr(count), value)
            for field in ('values', 'reports'):
                value = copy.deepcopy(baseline); del value[field]
                validate('missing '+field, value)
            value = copy.deepcopy(baseline); value['values'].pop(); validate('truncated vector', value)
            for kind in (0, 1, 2, 3):
                candidates = [i for i, v in enumerate(baseline['values']) if v['type'] == kind]
                if not candidates:
                    continue
                index = candidates[0]
                for number in (None, False, True, -1, 0, 1, 1.5, 2**32-1, 2**32, 2**64-1, 2**64, '1'):
                    value = copy.deepcopy(baseline); value['values'][index]['value'] = number
                    validate(f'value type {kind} {number}', value)
            for kind in (-1, 4, True, 0.0, None):
                value = copy.deepcopy(baseline); value['values'][0]['type'] = kind
                validate('invalid storage type '+repr(kind), value)
            for index, definition in enumerate(metadata['information']):
                for number in (True, False, None):
                    value = copy.deepcopy(baseline)
                    value['values'][len(metadata['metrics'])+index]['value'] = number
                    validate('flag '+definition['name']+' '+repr(number), value)
    finally:
        device.close()
    request, response = args.out/'request.json', args.out/'response.json'
    request.write_text(json.dumps(jobs), encoding='utf-8')
    env = os.environ.copy()
    if args.isolated_env:
        env = {key: value for key, value in env.items() if key.upper() in
               {'SYSTEMROOT', 'SYSTEMDRIVE', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA'}}
        windows = Path(os.environ['SystemRoot'])
        env['PATH'] = str(windows / 'System32') + os.pathsep + str(windows)
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env['PATH']
    subprocess.run([str(args.exe.resolve()), '--probe', str(request.resolve()), str(response.resolve())], env=env, check=True, timeout=180)
    actual = json.loads(response.read_text(encoding='utf-8'))
    assert len(actual) == len(expected)
    mismatches = [dict(index=i, label=labels[i], expected=e, actual=a) for i, (e, a) in enumerate(zip(expected, actual)) if e != a]
    result = dict(passed=not mismatches, cases=len(jobs), saved_reports=sum(job['op'] == 'decode' for job in jobs), mismatches=mismatches,
                  exe_sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(),
                  bridge_sha256=hashlib.sha256(args.bridge.read_bytes()).hexdigest())
    (args.out/'validation.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(dict(passed=result['passed'], cases=result['cases'], mismatches=len(mismatches))))
    if mismatches:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
