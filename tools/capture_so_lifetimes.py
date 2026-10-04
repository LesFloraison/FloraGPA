"""Serial development-only original SO fixtures; refuses to reuse an output root."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--shim', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    exe, shim, root = args.exe.resolve(), args.shim.resolve(), args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    frozen = root / 'producer'
    frozen.mkdir()
    repo = Path(__file__).resolve().parents[1]
    sources = ['tools/capture_so_lifetimes.py', 'tools/native/so_lifetime_probe.cpp',
               'tools/native/original_capture_control.h', 'tools/native/texture_probe_helpers.h',
               'tools/native/present_capture_probe.cpp']
    for source in sources:
        shutil.copy2(repo / source, frozen / Path(source).name)
    shutil.copy2(exe, frozen / exe.name)
    manifest = {'schema': 'FloraGPA SO lifetime producer evidence 1',
                'producer_files': {p.name: digest(p) for p in frozen.iterdir()},
                'original_dlls': {name: digest(shim.parent / name)
                                  for name in [shim.name, 'shimd3d64.dll']}, 'cases': []}
    for mode in range(9):
        directory = root / str(mode)
        directory.mkdir()
        capture = directory / 'capture.gpa_frame'
        case = {'mode': mode, 'runs': {}}
        manifest['cases'].append(case)
        for kind in ['native', 'captured']:
            env = os.environ.copy()
            env.pop('GPA_LOCAL_INJECT', None)
            output = directory / kind
            command = [str(exe), str(output), str(mode)]
            if kind == 'captured':
                command += [str(capture), str(shim)]
                env['GPA_LOCAL_INJECT'] = 'true'
            with (directory / (kind + '.log')).open('wb') as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=60, env=env)
            case['runs'][kind] = {'exit_code': result.returncode, 'command': command,
                                 'files': {p.name: digest(p) for p in output.iterdir() if p.is_file()}}
            (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
            assert result.returncode == 0, (mode, kind, result.returncode)
            oracle = json.loads((output / 'oracle.json').read_text())
            assert oracle['completed'] and oracle['identity_guard_checks'] == 2
            assert len(oracle['frames']) == 12
            for frame in oracle['frames']:
                assert frame['image_verified'] and frame['present_status'] == 0
                assert frame['getter_checks'] == (2 if mode in [3, 6] else 1)
                assert frame['storage_verified'] == (mode >= 5)
                assert frame['so_primitives_written'] == frame['so_primitives_needed'] == (1 if mode >= 6 else 0)
            assert (output / 'frame.rgba').read_bytes() == (output / 'expected.rgba').read_bytes()
        assert capture.stat().st_size > 0
        case['capture_sha256'] = digest(capture)
        for name in ['frame.rgba'] + (['so.bin'] if mode >= 5 else []):
            assert (directory / 'native' / name).read_bytes() == (directory / 'captured' / name).read_bytes()
        (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print('SO lifetime mode', mode, 'verified', flush=True)
    manifest['completed'] = True
    (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
