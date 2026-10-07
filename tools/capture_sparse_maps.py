"""Development-only original capture discovery for sparse mapped resource writes.

Modes 0..5 have marker images and require full resource-byte oracles. Modes 6..10
also draw from an earlier buffer copy. Never used by FloraGPA runtime.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inspect_capture(path):
    data = path.read_bytes()
    if len(data) < 0x128 or data[:4] != b'IGPA' or data[0x44:0x48] != b'DX11':
        raise ValueError('Expected original legacy DX11 capture')
    count = struct.unpack_from('<I', data, 12)[0]
    table = struct.unpack_from('<Q', data, 0xf4)[0]
    if table > len(data) or count > (len(data) - table) // 24:
        raise ValueError('Invalid capture index length')
    entries = {}
    for i in range(count):
        identity, offset, size, _, category, kind = struct.unpack_from('<QQIBBH', data, table + i * 24)
        if offset > len(data) or size > len(data) - offset or identity in entries:
            raise ValueError('Invalid capture entry')
        entries[identity] = (category, kind, data[offset:offset + size])
    writes = []
    for identity, (category, kind, raw) in sorted(entries.items()):
        if category != 7 or kind != 0x246:
            continue
        if len(raw) != 48:
            raise ValueError('Unexpected Map write envelope')
        link, context, hr, resource, subresource, map_type, flags, payload = struct.unpack('<QQiQIIIQ', raw)
        dc, dt, body = entries[payload]
        if link or hr or flags or dc != 9 or dt not in (1, 0x100):
            raise ValueError('Unverified Map write layout')
        row = dict(event=identity, context=context, resource=resource, subresource=subresource,
                   map_type=map_type, data_id=payload, data_type=dt, data_payload_bytes=len(body),
                   data_sha256=hashlib.sha256(body).hexdigest())
        if dt == 1:
            if len(body) < 4 or struct.unpack_from('<I', body)[0] != len(body) - 4:
                raise ValueError('Invalid full Map data length')
            row['storage_sha256'] = hashlib.sha256(body[4:]).hexdigest()
        else:
            if len(body) < 8:
                raise ValueError('Truncated differential header')
            headers, size = struct.unpack_from('<II', body)
            if headers % 8 or headers + size != len(body) - 8:
                raise ValueError('Invalid differential header')
            row['ranges'] = [list(struct.unpack_from('<II', body, 8 + n)) for n in range(0, headers, 8)]
            if sum(n for _, n in row['ranges']) != size:
                raise ValueError('Invalid differential data length')
        writes.append(row)
    if len(writes) != 2 or writes[0]['resource'] != writes[1]['resource']:
        raise ValueError('Original capture did not save both writes to the target resource')
    return writes


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--producer', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--gpa-dir', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--modes', type=int, nargs='+', default=list(range(8)))
    args = parser.parse_args()
    if len(set(args.modes)) != len(args.modes) or any(n < 0 or n > 10 for n in args.modes):
        parser.error('Choose distinct modes 0..10')
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[1]
    frozen = root / 'producer'
    frozen.mkdir()
    for name in ['tools/native/sparse_map_probe.cpp', 'tools/native/present_capture_probe.cpp',
                 'tools/native/original_capture_control.h', 'tools/capture_sparse_maps.py']:
        shutil.copy2(repo / name, frozen / Path(name).name)
    shutil.copy2(args.producer.resolve(strict=True), frozen / args.producer.name)
    exe = frozen / args.producer.name
    manifest = dict(schema='FloraGPA sparse Map discovery 1', completed=False,
                    producer_files={p.name: sha(p) for p in frozen.iterdir()},
                    gpa_sha256={name: sha(args.gpa_dir / name) for name in
                                ['shimloader64.dll', 'shimd3d64.dll', 'dx11_player.dll']},
                    producer_runs=[], cases=[])

    def save():
        (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')

    try:
        for mode in args.modes:
            folder = root / str(mode)
            folder.mkdir()
            capture = folder / 'capture.gpa_frame'
            for backend in ['hardware', 'warp', 'captured']:
                output = folder / backend
                command = [str(exe), str(output), str(mode)]
                env = dict(os.environ)
                env.pop('GPA_LOCAL_INJECT', None)
                env.pop('FLORA_SPARSE_MAP_WARP', None)
                env.pop('FLORA_SPARSE_MAP_DEBUG', None)
                if backend == 'warp':
                    env['FLORA_SPARSE_MAP_WARP'] = '1'
                if backend == 'captured':
                    env['GPA_LOCAL_INJECT'] = 'true'
                    command += [str(capture), str(args.gpa_dir / 'shimloader64.dll')]
                with (folder / (backend + '.log')).open('wb') as log:
                    result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                            creationflags=subprocess.CREATE_NO_WINDOW, timeout=60)
                row = dict(mode=mode, backend=backend, exit_code=result.returncode)
                manifest['producer_runs'].append(row)
                save()
                if result.returncode:
                    raise RuntimeError(f'Producer failed: {mode}/{backend}')
                oracle = json.loads((output / 'oracle.json').read_text())
                row['oracle'] = oracle
                if not (oracle['completed'] and len(oracle['frames']) == 12 and not oracle['failures']
                        and oracle['warp'] == (backend == 'warp')
                        and oracle['capture_requested'] == (backend == 'captured')):
                    raise RuntimeError('Producer mode or completion mismatch')
                expected = (output / 'after.bin').read_bytes()
                before = (output / 'before.bin').read_bytes()
                if any((output / f'before-{n}.bin').read_bytes() != before for n in range(12)):
                    raise RuntimeError('Native reset byte mismatch')
                if any((output / f'actual-{n}.bin').read_bytes() != expected for n in range(12)):
                    raise RuntimeError('Native sparse write byte mismatch')
                if expected != (folder / 'hardware/after.bin').read_bytes():
                    raise RuntimeError('Producer oracle differs between backends')
                if mode >= 6:
                    for n in range(12):
                        if (output / f'image-{n}.rgba').read_bytes() != before[:4] * 64:
                            raise RuntimeError('GPU image does not preserve the earlier copy snapshot')
                    row['images'] = {p.name: sha(p) for p in sorted(output.glob('*.rgba'))}
                row['raw_bytes'] = {p.name: sha(p) for p in sorted(output.glob('*.bin'))}
                save()
            writes = inspect_capture(capture)
            manifest['cases'].append(dict(mode=mode, path=f'{mode}/capture.gpa_frame', sha256=sha(capture),
                                         bytes=capture.stat().st_size, writes=writes,
                                         before_sha256=sha(folder / 'hardware/before.bin'),
                                         after_sha256=sha(folder / 'hardware/after.bin')))
            save()
            print(mode, [w['data_type'] for w in writes], flush=True)
        manifest['completed'] = True
    finally:
        save()


if __name__ == '__main__':
    main()
