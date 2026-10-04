"""Serial development-only Discard originals, with defined storage/image CPU oracles."""
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
    for name in ['exe', 'shim', 'out']:
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    exe, shim, root = args.exe.resolve(strict=True), args.shim.resolve(strict=True), args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    frozen = root / 'producer'
    frozen.mkdir()
    repo = Path(__file__).resolve().parents[1]
    for source in ['tools/capture_discards.py', 'tools/native/discard_probe.cpp',
                   'tools/native/original_capture_control.h', 'tools/native/texture_probe_helpers.h',
                   'tools/native/present_capture_probe.cpp']:
        shutil.copy2(repo / source, frozen / Path(source).name)
    shutil.copy2(exe, frozen / exe.name)
    manifest = dict(schema='FloraGPA discard producer evidence 1', completed=False,
                    producer_files={p.name: sha(p) for p in frozen.iterdir()},
                    original_dlls={name: sha(shim.parent / name) for name in [shim.name, 'shimd3d64.dll']}, cases=[])

    def save():
        (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')

    for mode in range(17):
        directory = root / str(mode)
        directory.mkdir()
        capture = directory / 'capture.gpa_frame'
        case = dict(mode=mode, runs={})
        manifest['cases'].append(case)
        for kind in ['native', 'captured']:
            output = directory / kind
            command = [str(exe), str(output), str(mode)]
            env = os.environ.copy()
            env.pop('GPA_LOCAL_INJECT', None)
            if kind == 'captured':
                command += [str(capture), str(shim)]
                env['GPA_LOCAL_INJECT'] = 'true'
            record = dict(command=command)
            case['runs'][kind] = record
            save()
            try:
                with (directory / (kind + '.log')).open('wb') as stream:
                    result = subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=60)
                record['exit_code'] = result.returncode
                record['files'] = {p.name: sha(p) for p in output.iterdir() if p.is_file()}
                if result.returncode:
                    raise RuntimeError((output / 'error.txt').read_text() if (output / 'error.txt').exists() else 'See process log')
                oracle = json.loads((output / 'oracle.json').read_text())
                if not oracle['completed'] or len(oracle['frames']) != 12:
                    raise RuntimeError('Incomplete producer frames')
                if not all(f['storage_verified'] and f['image_verified'] for f in oracle['frames']):
                    raise RuntimeError('Incomplete producer oracles')
                for actual, expected in [('actual.bin', 'expected.bin'), ('frame.rgba', 'expected.rgba')]:
                    if (output / actual).read_bytes() != (output / expected).read_bytes():
                        raise RuntimeError('CPU oracle mismatch: ' + actual)
                if kind == 'captured':
                    for name in ['expected.bin', 'expected.rgba']:
                        if (output / name).read_bytes() != (directory / 'native' / name).read_bytes():
                            raise RuntimeError('Native/injected CPU oracle disagreement')
            except (RuntimeError, subprocess.TimeoutExpired) as error:
                record['failure'] = str(error)
                print(mode, kind, 'failed:', error, flush=True)
                save()
                break
            save()
        case['accepted_producer'] = len(case['runs']) == 2 and all('failure' not in r for r in case['runs'].values())
        if case['accepted_producer']:
            if not capture.exists() or not capture.stat().st_size:
                raise RuntimeError('Missing delivered capture')
            case['capture_sha256'] = sha(capture)
            print('Discard mode', mode, 'verified', flush=True)
        save()
    manifest['completed'] = True
    manifest['all_producers_verified'] = all(c['accepted_producer'] for c in manifest['cases'])
    save()
    if not manifest['all_producers_verified']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
