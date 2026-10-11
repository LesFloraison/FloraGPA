"""Development experiment only: resource renaming is not query capture support."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


def sha(data):
    return hashlib.sha256(data).hexdigest()


def entries(raw):
    if raw[:4] != b'IGPA' or len(raw) < 260:
        raise ValueError('Invalid experiment input')
    count = struct.unpack_from('<I', raw, 12)[0]
    table = struct.unpack_from('<Q', raw, 244)[0]
    if table > len(raw) or count > (len(raw)-table)//24:
        raise ValueError('Invalid experiment index')
    result = {}
    for i in range(count):
        identity, offset, size, flags, category, kind = struct.unpack_from('<QQIBBH', raw, table+i*24)
        if identity in result or offset > len(raw) or size > len(raw)-offset or flags:
            raise ValueError('Invalid experiment record')
        result[identity] = (offset, size, category, kind)
    return result


def variants(raw):
    records = entries(raw)
    maps = [(identity, offset) for identity, (offset, size, category, kind) in records.items()
            if category == 7 and kind == 0x246 and size == 48
            and struct.unpack_from('<I', raw, offset+32)[0] == 5]
    if len(maps) != 1:
        raise ValueError('Expected one captured NO_OVERWRITE')
    identity, offset = maps[0]
    link, context, hr, resource, sub, kind, flags, data = struct.unpack_from('<QQiQIIIQ', raw, offset)
    rpos, rsize, rcat, rtype = records[resource]
    dpos, dsize, dcat, dtype = records[data]
    # This experiment is only valid with a complete recorded buffer payload.
    # Applying DISCARD to an arbitrary sparse patch would destroy untouched data.
    width, usage, bind, access, misc, stride = struct.unpack_from('<6I', raw, rpos+16)
    if not (link == 0 and context and hr == 0 and sub == 0 and flags == 0
            and (rcat, rtype, rsize) == (5, 0x83, 48) and usage == 2 and bind == 1
            and access == 0x10000 and misc == stride == 0 and (dcat, dtype) == (9, 1)
            and struct.unpack_from('<I', raw, dpos)[0] == width and dsize == width+4):
        raise ValueError('Input is not the pinned full dynamic vertex-buffer mapping')
    renamed = bytearray(raw)
    struct.pack_into('<I', renamed, offset+32, 4)
    if [i for i, (a, b) in enumerate(zip(raw, renamed)) if a != b] != [offset+32]:
        raise AssertionError('Renaming changed more than one Map type byte')
    draws = [(at, size) for at, size, category, kind in records.values() if category == 7 and kind == 0x37]
    if len(draws) != 1 or draws[0][1] != 32:
        raise ValueError('Expected one Draw in the original completion fixture')
    negative = bytearray(renamed)
    struct.pack_into('<I', negative, draws[0][0]+24, 0)
    # Pinned producer order: Copy at 9, incidental nested GetDesc at 10,
    # then EVENT polls. Replace only that metadata slot for parser experiments.
    # These are authored variants, not an assertion that the original saved End.
    at, size, category, kind = records[10]
    if (size, category, kind) != (21, 7, 0x3019) or struct.unpack_from('<Q', raw, at)[0] != 9:
        raise ValueError('Expected disposable nested GetDesc metadata is absent')
    queries = {struct.unpack_from('<Q', raw, at+20)[0] for at, size, category, kind in records.values()
               if category == 7 and kind == 0x34fb}
    if len(queries) != 1 or next(iter(queries)) in records:
        raise ValueError('Expected one omitted ordinary Query resource')
    query = next(iter(queries))
    table = struct.unpack_from('<Q', raw, 244)[0]
    slot = next(table+i*24 for i in range(len(records)) if struct.unpack_from('<Q', raw, table+i*24)[0] == 10)
    payload = struct.pack('<QQQ', 0, context, query)
    end_api = bytearray(renamed[:table]+payload+renamed[table:])
    # Checked containers require every record to precede the index table.
    # Original pooled data stays in place; only the index and BMP shift.
    preview = struct.unpack_from('<Q', raw, 276)[0]
    if preview < table:
        raise ValueError('Unexpected original preview position')
    struct.pack_into('<Q', end_api, 244, table+len(payload))
    struct.pack_into('<Q', end_api, 276, preview+len(payload))
    slot += len(payload)
    struct.pack_into('<QI', end_api, slot+8, table, len(payload))
    struct.pack_into('<H', end_api, slot+22, 0x34fa)
    end_erg = bytearray(end_api)
    struct.pack_into('<H', end_erg, slot+22, 0x243)
    return dict(original=raw, renamed=bytes(renamed), no_draw=bytes(negative),
                end_api=bytes(end_api), end_erg=bytes(end_erg)), dict(
        map_event=identity, resource=resource, data_record=data, full_bytes=width,
        map_type_byte=offset+32, original_map_type=5, experimental_map_type=4,
        replaced_metadata_event=10, omitted_query=query)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('corpus', 'replay', 'oracle-tools', 'qt-bin', 'out'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--repeat', type=int, default=3)
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
                  inputs={str(p): sha(p.read_bytes()) for p in
                          (Path(__file__), args.replay, args.oracle_tools/'replay_frame.py')},
                  original_adapter_verified=False, mutations={}, runs=[])

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
            versions, mutation = variants(raw)
            report['mutations'][str(mode)] = mutation
            for name, data in versions.items():
                frame = out/f'{mode}-{name}.gpa_frame'
                with frame.open('xb') as file:
                    file.write(data)
                for repeat in range(1 if name.startswith('end_') else args.repeat):
                    for backend in ('flora', 'original'):
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
                        if name == 'end_erg' or (name == 'end_api' and backend == 'flora'):
                            diagnostic = result.stdout+result.stderr
                            if backend == 'original' and (folder/'worker.log').exists():
                                diagnostic += (folder/'worker.log').read_bytes()
                            expected_error = (b'Open failed: status=13' if backend == 'original'
                                              else f'Predicate resource {mutation["omitted_query"]} is absent'.encode())
                            if not result.returncode or expected_error not in diagnostic:
                                raise AssertionError(prefix+' did not reject the missing query as expected')
                            run['expected_rejection'] = True
                            save()
                            continue
                        if result.returncode:
                            raise RuntimeError(prefix+' did not execute')
                        if backend == 'flora':
                            native = json.loads((folder/'report.json').read_text())
                            actual = sha((folder/'frame.rgba').read_bytes())
                            if native['reference_pixels_used']:
                                raise AssertionError('Reference substituted for execution')
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
                        if name != 'original' and actual != target:
                            raise AssertionError(prefix+' changed application pixels')
        report['original_mismatches'] = sum(not r['exact'] for r in report['runs'] if r['variant'] == 'original')
        if not report['original_mismatches']:
            raise AssertionError('Missing-boundary problem was not reproduced')
        report['passed'] = True
    finally:
        save()
    print(json.dumps(dict(passed=True, runs=len(report['runs']), original_mismatches=report['original_mismatches'])))


if __name__ == '__main__':
    main()
