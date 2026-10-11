"""Development-only experiment: executable READ Map is not a general EVENT wait."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

from validate_capture_query_renaming import entries, sha


def variants(raw):
    records = entries(raw)
    # Preserve every captured command, resource descriptor and data byte except
    # the pinned incidental nested GetDesc slot. No NO_OVERWRITE renaming.
    at, size, category, kind = records[10]
    if (size, category, kind) != (21, 7, 0x3019) or struct.unpack_from('<Q', raw, at)[0] != 9:
        raise ValueError('Expected nested GetDesc slot is absent')
    at, size, category, kind = records[9]
    if (size, category, kind) != (32, 7, 0x3e):
        raise ValueError('Expected preceding CopyResource is absent')
    parent, context, destination, source = struct.unpack_from('<4Q', raw, at)
    at, size, category, kind = records[destination]
    native, device, width, usage, bind, access, misc, stride, initial = struct.unpack_from('<QQ6IQ', raw, at)
    if not (parent == 0 and (size, category, kind) == (48, 5, 0x83)
            and width == 65536 and usage == 3 and bind == 0 and access == 0x20000 and misc == stride == 0):
        raise ValueError('Expected unmodified staging READ destination is absent')
    table = struct.unpack_from('<Q', raw, 244)[0]
    preview = struct.unpack_from('<Q', raw, 276)[0]
    count = len(records)
    if preview < table+count*24:
        raise ValueError('Unexpected preview/index position')
    data_id = max(records)+1
    result = {}
    for name in ('no_data', 'empty_diff', 'no_draw'):
        payload = struct.pack('<QQiQIIIQ', 0, context, 0, destination, 0, 1, 0,
                              0 if name == 'no_data' else data_id)
        # GenDataDiff with zero ranges and zero bytes makes no CPU writes to the
        # read-only mapping. The nonzero data reference enables the native Map.
        payload += struct.pack('<II', 0, 0)
        new_table = table+len(payload)
        data = bytearray(raw[:table]+payload+raw[table:table+count*24]
                         +struct.pack('<QQIBBH', data_id, table+48, 8, 0, 9, 0x100)
                         +raw[table+count*24:])
        struct.pack_into('<I', data, 12, count+1)
        struct.pack_into('<Q', data, 244, new_table)
        struct.pack_into('<Q', data, 276, preview+len(payload)+24)
        slot = next(new_table+i*24 for i in range(count)
                    if struct.unpack_from('<Q', data, new_table+i*24)[0] == 10)
        struct.pack_into('<QI', data, slot+8, table, 48)
        struct.pack_into('<H', data, slot+22, 0x246)
        draws = [(a, n) for a, n, c, t in records.values() if c == 7 and t == 0x37]
        if len(draws) != 1 or draws[0][1] != 32:
            raise ValueError('Expected single Draw is absent')
        if name == 'no_draw':
            struct.pack_into('<I', data, draws[0][0]+24, 0)
        revised = entries(data)
        if set(revised) != set(records) | {data_id}:
            raise AssertionError('Record identities changed')
        for identity, (a, n, c, t) in records.items():
            if identity == 10 or (name == 'no_draw' and a == draws[0][0]):
                continue
            b, m, rc, rt = revised[identity]
            if (n, c, t) != (m, rc, rt) or raw[a:a+n] != data[b:b+m]:
                raise AssertionError('Original captured record changed')
        if raw[preview:] != data[preview+len(payload)+24:]:
            raise AssertionError('Captured preview changed')
        result[name] = bytes(data)
    return result, dict(inserted_map_event=10, preceding_copy_event=9, context=context,
                        mapped_destination=destination, source=source, data_id=data_id,
                        map_type='READ', nonzero_empty_diff=True, original_maps_unchanged=True,
                        query_observations_unchanged=True, general_event_wait=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('corpus', 'replay', 'oracle-tools', 'qt-bin', 'out'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--read-map-supported', action='store_true', help='Require verified empty-diff READ execution in FloraGPA')
    args = parser.parse_args()
    if not 1 <= args.repeat <= 20:
        parser.error('Repeat must be 1..20')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env.pop('GPA_LOCAL_INJECT', None)
    env['PATH'] = str(args.qt_bin.resolve())+os.pathsep+env['PATH']
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    pinned = {8: '39510aa38da0c512da4f9f3dd0cb2e71915b9254f85d8a8cd261f8bde59d4747',
              9: '0cb5a893307d8f47d44ea2e239ebfbee30df0354687ba0a1a8a9bddedaa4dbeb'}
    report = dict(passed=False, scope=__doc__, production_query_support=False,
                  flora_read_map_profile=args.read_map_supported,
                  original_adapter_verified=False, inputs={str(p): sha(p.read_bytes()) for p in
                    (Path(__file__), Path(__file__).with_name('validate_capture_query_renaming.py'),
                     args.replay, args.oracle_tools/'replay_frame.py')}, mutations={}, runs=[])

    def save():
        (out/'validation.json').write_text(json.dumps(report, indent=2)+'\n')

    try:
        for mode in (8, 9):
            source = args.corpus/str(mode)/'capture.gpa_frame'
            raw = source.read_bytes()
            if sha(raw) != pinned[mode]:
                raise ValueError('Original query fixture hash mismatch')
            report['inputs'][str(source)] = sha(raw)
            expected = (args.corpus/str(mode)/'hardware/image-0.rgba').read_bytes()
            if sha(expected) != 'e887048c7cb81839843ace2938854ff7f749dfb6cf8e6235ed54c0ce9ce4b232':
                raise ValueError('Independent application image hash mismatch')
            versions, report['mutations'][str(mode)] = variants(raw)
            for name, data in versions.items():
                frame = out/f'{mode}-{name}.gpa_frame'
                with frame.open('xb') as file:
                    file.write(data)
                for backend in ('flora', 'original'):
                    for repeat in range(args.repeat if backend == 'original' or (args.read_map_supported and name != 'no_data') else 1):
                        prefix = f'{mode}-{name}-{repeat}-{backend}'
                        folder = out/prefix
                        command = ([args.replay.resolve(), 'replay', frame, '--out', folder]
                                   if backend == 'flora' else
                                   [sys.executable, args.oracle_tools.resolve()/'replay_frame.py', frame,
                                    '--out', folder, '--timeout', '60'])
                        run = dict(mode=mode, variant=name, repeat=repeat, backend=backend, frame_sha256=sha(data))
                        report['runs'].append(run)
                        save()
                        result = subprocess.run([str(v) for v in command], env=env, capture_output=True, timeout=90)
                        (out/(prefix+'.log')).write_bytes(result.stdout+b'\n'+result.stderr)
                        run['exit_code'] = result.returncode
                        if backend == 'flora' and (not args.read_map_supported or name == 'no_data'):
                            diagnostic = b'Successful Map has no captured data identity' if args.read_map_supported else b'Captured write record contains a READ Map'
                            if not result.returncode or diagnostic not in result.stdout+result.stderr:
                                raise AssertionError(prefix+' did not preserve the expected Map guard')
                            run['expected_rejection'] = True
                            save()
                            continue
                        if result.returncode:
                            raise RuntimeError(prefix+' did not execute')
                        if backend == 'flora':
                            native = json.loads((folder/'report.json').read_text())
                            actual = sha((folder/'frame.rgba').read_bytes())
                            if native['reference_pixels_used'] or native['counts'].get('map_read_erg_synchronizations') != 1:
                                raise AssertionError('FloraGPA did not execute the READ dependency')
                            run['unresolved_query_completions'] = native['counts'].get('query_completion_unresolved', 0)
                        else:
                            native = json.loads((folder/'replay_report.json').read_text())
                            actual = native['replayed_rgba_sha256']
                            if not (native['new_framebuffer_count'] > 0 and native['native']['open_status'] == 0
                                    and native['native']['playback_status'] == 0 and native['native']['closed']):
                                raise AssertionError('Original player did not execute')
                            player_sha = native['native']['player_sha256']
                            if player_sha != '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b':
                                raise AssertionError('Original player version changed')
                            run['player_sha256'] = player_sha
                        target = sha(bytes([0, 255, 0, 255])*64 if name == 'no_draw' else expected)
                        run.update(actual_sha256=actual, expected_sha256=target, exact=actual == target)
                        save()
                        if name != 'no_data' and actual != target:
                            raise AssertionError(prefix+' changed application pixels')
        report['no_data_mismatches'] = sum(not r['exact'] for r in report['runs']
                                         if r['variant'] == 'no_data' and r['backend'] == 'original')
        if not report['no_data_mismatches']:
            raise AssertionError('Nonexecuting Map control did not reproduce the missing wait')
        report['passed'] = True
    finally:
        save()
    print(json.dumps(dict(passed=True, runs=len(report['runs']), no_data_mismatches=report['no_data_mismatches'])))


if __name__ == '__main__':
    main()
