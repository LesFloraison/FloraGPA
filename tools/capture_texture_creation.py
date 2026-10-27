"""Regenerate development-only original GPA texture creation fixtures in a new directory."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from capture_present import digest, require


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--producer', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--gpa-dir', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    args = parser.parse_args()
    producer = args.producer.resolve(strict=True)
    gpa = args.gpa_dir.resolve(strict=True)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sources = Path(__file__).parent
    manifest = dict(schema=1, scope='owned_original_texture_creation_corpus', completed=False,
                    producer_sha256=digest(producer), cases=[], runs=[],
                    source_sha256={name: digest(sources / name) for name in
                                   ('capture_texture_creation.py', 'capture_present.py',
                                    'native/texture_creation_probe.cpp', 'native/texture_copy_probe.cpp',
                                    'native/present_capture_probe.cpp')},
                    gpa_sha256={name: digest(gpa / name) for name in ('shimloader64.dll', 'shimd3d64.dll')})

    def save():
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')

    try:
        for mode in list(range(8)) + list(range(10, 17)) + list(range(30, 39)):
            folder = out / str(mode)
            folder.mkdir()
            capture = folder / 'capture.gpa_frame'
            for backend in ('native', 'captured'):
                env = dict(os.environ)
                env.pop('GPA_LOCAL_INJECT', None)
                command = [str(producer), str(folder / backend), str(mode)]
                if backend == 'captured':
                    command += [str(capture), str(gpa / 'shimloader64.dll')]
                    env['GPA_LOCAL_INJECT'] = 'true'
                with (folder / (backend + '.log')).open('xb') as log:
                    result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                            creationflags=subprocess.CREATE_NO_WINDOW, timeout=60)
                record = dict(mode=mode, backend=backend, command=command, exit_code=result.returncode)
                manifest['runs'].append(record)
                save()
                require(result.returncode == 0, f'{mode}/{backend}: producer failed')
                oracle = json.loads((folder / backend / 'oracle.json').read_text())
                require(oracle['completed'] and len(oracle['frames']) == 12, 'Incomplete producer')
                require(all(f['bytes_verified'] and f['image_verified'] for f in oracle['frames']),
                        'Producer verification failed')
                record['oracle'] = oracle
                for name in ('initial.bin', 'expected.bin', 'texture.bin', 'frame.rgba'):
                    data = (folder / backend / name).read_bytes()
                    record[name + '_sha256'] = digest(folder / backend / name)
                    if backend == 'captured':
                        require(data == (folder / 'native' / name).read_bytes(), 'Native/shim storage differs: ' + name)
                require((folder / backend / 'texture.bin').read_bytes() ==
                        (folder / backend / 'expected.bin').read_bytes(), 'Final bytes differ')
                require((folder / backend / 'frame.rgba').read_bytes() == bytes([0, 255, 0, 255]) * 64,
                        'Texture-dependent image differs')
                save()
            require(capture.is_file() and capture.stat().st_size > 0, 'Missing original capture')
            manifest['cases'].append(dict(mode=mode, path=str(capture.relative_to(out)), sha256=digest(capture)))
            save()
            print(f'{mode}: verified native and original capture', flush=True)
        manifest['completed'] = True
    except Exception as error:
        manifest['error'] = str(error)
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
