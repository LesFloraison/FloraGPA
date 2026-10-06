"""Development-only original GPA captures of occluded blt-model Present TEST.

Reject files that omit the tested status. A requested capture alone does not
prove that GPA serialized the target operation. Never used by FloraGPA runtime.
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
    records = []
    for i in range(count):
        identity, offset, size, flags, category, kind = struct.unpack_from('<QQIBBH', data, table + i * 24)
        if offset > len(data) or size > len(data) - offset:
            raise ValueError('Invalid capture payload range')
        records.append((identity, category, kind, data[offset:offset + size]))
    tests = [(identity, struct.unpack('<QQiII', payload)) for identity, category, kind, payload in records
             if category == 7 and kind == 0x3257 and len(payload) == 28
             and struct.unpack_from('<iII', payload, 16) == (0x087a0001, 0, 1)]
    if len(tests) != 1:
        raise ValueError('Capture does not contain exactly one saved occluded Present TEST')
    event, (_, chain, _, _, _) = tests[0]
    storage = [identity for identity, category, kind, payload in records
               if category == 5 and kind == 0x85 and len(payload) == 68
               and struct.unpack_from('<Q', payload, 8)[0] == chain]
    green = [identity for identity, category, kind, payload in records
             if category == 7 and kind == 0x32 and identity > event and len(payload) == 41
             and struct.unpack_from('<ffff', payload, 25) == (0, 1, 0, 1)]
    prior = [identity for identity, category, _, _ in records if category == 7 and identity < event]
    if len(storage) != 1 or len(green) != 1 or not prior:
        raise ValueError('Missing unique buffer, preceding event or later green-clear witness')
    return event, chain, storage[0], max(prior), green[0]


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--producer', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--gpa-dir', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    args = parser.parse_args()
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    frozen = root / 'producer'
    frozen.mkdir()
    repo = Path(__file__).resolve().parents[1]
    for name in ['tools/native/present_capture_probe.cpp', 'tools/native/original_capture_control.h',
                 'tools/capture_occluded_present.py']:
        shutil.copy2(repo / name, frozen / Path(name).name)
    shutil.copy2(args.producer.resolve(strict=True), frozen / args.producer.name)
    exe = frozen / args.producer.name
    manifest = dict(schema='FloraGPA compatibility corpus 1', profile='GPA 2025 R1 legacy DX11 / IGPA v3',
                    completed=False, producer_files={p.name: sha(p) for p in frozen.iterdir()},
                    gpa_sha256={name: sha(args.gpa_dir / name) for name in
                                ['shimloader64.dll', 'shimd3d64.dll', 'dx11_player.dll']},
                    producer_runs=[], cases=[])
    red, green = bytes([255, 0, 0, 255]) * 64, bytes([0, 255, 0, 255]) * 64
    red_sha, green_sha = (hashlib.sha256(v).hexdigest() for v in (red, green))

    def save():
        (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')

    try:
        for mode in [5, 7]:
            folder = root / str(mode)
            folder.mkdir()
            capture = folder / 'capture.gpa_frame'
            for backend in ['hardware', 'warp', 'captured']:
                output = folder / backend
                command = [str(exe), str(output), str(mode)]
                env = dict(os.environ)
                env.pop('GPA_LOCAL_INJECT', None)
                env.pop('FLORA_PRESENT_WARP', None)
                if backend == 'warp':
                    env['FLORA_PRESENT_WARP'] = '1'
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
                if not (oracle['completed'] and len(oracle['frames']) == 12 and oracle['pixel_failures'] == 0
                        and oracle['warp'] == (backend == 'warp')
                        and oracle['capture_requested'] == (backend == 'captured')):
                    raise RuntimeError('Incomplete producer or incorrect execution mode')
                for f in oracle['frames']:
                    if not (f['hresult'] == 0x087a0001 and f['flags'] == 1 and f['minimized']
                            and f['before'] and f['after'] and f['terminal_hresult'] == 0):
                        raise RuntimeError('Unexpected native Present status/binding')
                    for phase in ['before', 'after']:
                        if (output / f'{phase}-{f["frame"]}.rgba').read_bytes() != red:
                            raise RuntimeError('Present changed observed red storage')
                if (output / 'oracle.rgba').read_bytes() != green:
                    raise RuntimeError('Later green clear does not match the CPU oracle')
                row['raw_images'] = {p.name: sha(p) for p in sorted(output.glob('*.rgba'))}
                save()
            event, chain, storage, prior, clear = inspect_capture(capture)
            manifest['cases'].append(dict(
                id=f'present_occluded_test_{mode}', path=f'{mode}/capture.gpa_frame', sha256=sha(capture),
                bytes=capture.stat().st_size, family='present_occluded', origin='self_owned_original_gpa_capture',
                provenance_note='Unmodified original CaptureNextFrame output with pinned primary selector; saved occlusion status checked.',
                source_manifests=[], device_scope='Producer hardware/WARP adapter IDs recorded; original-player adapter unidentified.',
                reference_rgba_sha256=green_sha, comparison_policy='exact_golden',
                native_workload_fidelity='passed', application_reference_rgba_sha256=green_sha,
                application_reference_scope='Full 8x8 RGBA CPU oracle checked on uninjected hardware, WARP and shim-injected producer; TEST storage boundaries also checked.',
                application_reference_evidence=dict(path=f'{mode}/hardware/oracle.rgba', sha256=green_sha),
                fidelity_reason='Saved occluded TEST retains binding/storage and subsequent green clear matches the independent producer.',
                controls=[dict(disable_event=clear, repeat=2, expected_rgba_sha256=red_sha,
                               purpose='Omit later green clear; retain red storage across TEST')],
                boundaries=[dict(kind='texture', resource=storage, event=boundary, repeat=2,
                                 expect_stable=True, expected_storage_sha256=expected)
                            for boundary, expected in [(prior, red_sha), (event, red_sha), (clear, green_sha)]],
                covers=['Occluded blt-model Present TEST', 'Retained RTV/storage and later in-frame writes'],
                present_event=event, swap_chain=chain))
            save()
            print(mode, 'native, WARP and original captured status verified', flush=True)
        manifest['completed'] = True
    except Exception as error:
        manifest['error'] = str(error)
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
