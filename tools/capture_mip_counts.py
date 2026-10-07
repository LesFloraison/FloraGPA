"""Development-only original captures: mip-count queries versus resource LOD."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--producer', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--gpa-dir', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--modes', type=int, nargs='+', default=list(range(6)),
                        help='0..5 SM5, 6..11 SM4.0, 12..19 branches, 20..27 switches')
    args = parser.parse_args()
    if len(set(args.modes)) != len(args.modes) or any(n < 0 or n > 27 for n in args.modes):
        parser.error('Choose distinct modes 0..27')
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    frozen = root / 'producer'
    frozen.mkdir()
    repo = Path(__file__).resolve().parents[1]
    for name in ['tools/capture_mip_counts.py', 'tools/native/mip_count_probe.cpp',
                 'tools/native/texture_probe_helpers.h', 'tools/native/present_capture_probe.cpp',
                 'tools/native/original_capture_control.h']:
        shutil.copy2(repo / name, frozen / Path(name).name)
    shutil.copy2(args.producer.resolve(strict=True), frozen / args.producer.name)
    exe = frozen / args.producer.name
    manifest = dict(schema='FloraGPA mip-count original capture batch 1', completed=False,
                    producer_files={p.name: sha(p) for p in frozen.iterdir()},
                    gpa_sha256={n: sha(args.gpa_dir / n) for n in
                                ['shimloader64.dll', 'shimd3d64.dll', 'dx11_player.dll']},
                    producer_runs=[], captures=[])

    def save():
        (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')

    try:
        for mode in args.modes:
            folder = root / str(mode)
            folder.mkdir()
            capture = folder / 'capture.gpa_frame'
            for backend in ['hardware', 'warp', 'captured']:
                env = dict(os.environ)
                env.pop('GPA_LOCAL_INJECT', None)
                env.pop('FLORA_MINLOD_WARP', None)
                command = [str(exe), str(folder / backend), str(mode)]
                if backend == 'warp':
                    env['FLORA_MINLOD_WARP'] = '1'
                if backend == 'captured':
                    command += [str(capture), str(args.gpa_dir / 'shimloader64.dll')]
                    env['GPA_LOCAL_INJECT'] = 'true'
                row = dict(mode=mode, backend=backend, command=command)
                manifest['producer_runs'].append(row)
                save()
                with (folder / (backend + '.log')).open('wb') as log:
                    result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                            creationflags=subprocess.CREATE_NO_WINDOW, timeout=60)
                row['exit_code'] = result.returncode
                save()
                if result.returncode:
                    raise RuntimeError(f'Producer failed: {mode}/{backend}')
                oracle = json.loads((folder / backend / 'oracle.json').read_text())
                assert oracle['completed'] and len(oracle['frames']) == 12
                assert oracle['mode'] == mode and oracle['warp'] == (backend == 'warp')
                assert oracle['profile'] == ('ps_5_0' if mode < 6 else 'ps_4_0')
                assert oracle['capture_requested'] == (backend == 'captured')
                assert all(frame['image_verified'] for frame in oracle['frames'])
                assert (folder / backend / 'frame.rgba').read_bytes() == (folder / backend / 'expected.rgba').read_bytes()
                row['oracle'] = oracle
                row['image_sha256'] = sha(folder / backend / 'frame.rgba')
                assert row['image_sha256'] == sha(folder / 'hardware/expected.rgba')
                save()
            assert capture.is_file()
            manifest['captures'].append(dict(mode=mode, path=f'{mode}/capture.gpa_frame',
                                             sha256=sha(capture), bytes=capture.stat().st_size))
            save()
            print(mode, 'native hardware/WARP and original capture verified', flush=True)
        manifest['completed'] = True
    except Exception as error:
        manifest['error'] = str(error)
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
