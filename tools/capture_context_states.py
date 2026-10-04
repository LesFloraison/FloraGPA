"""Serial development-only context-state originals, with preserved failed attempts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(value, message):
    if not value:
        raise RuntimeError(message)


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
    for source in ['tools/capture_context_states.py', 'tools/native/context_state_probe.cpp',
                   'tools/native/original_capture_control.h', 'tools/native/texture_probe_helpers.h',
                   'tools/native/present_capture_probe.cpp']:
        shutil.copy2(repo / source, frozen / Path(source).name)
    shutil.copy2(exe, frozen / exe.name)
    manifest = dict(schema='FloraGPA context-state producer evidence 1', completed=False,
                    producer_files={p.name: sha(p) for p in frozen.iterdir()},
                    original_dlls={name: sha(shim.parent / name) for name in [shim.name, 'shimd3d64.dll']}, cases=[])

    def save():
        (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')

    sequences = [[0], [1], [1], [1], [1, 0], [1, 2, 0], [0], [1]]
    for mode, colors in enumerate(sequences):
        directory = root / str(mode)
        directory.mkdir()
        capture = directory / 'capture.gpa_frame'
        case = dict(mode=mode, colors=colors, runs={})
        manifest['cases'].append(case)
        for kind in ['native', 'captured']:
            output = directory / kind
            command = [str(exe), str(output), str(mode)]
            env = os.environ.copy()
            env.pop('GPA_LOCAL_INJECT', None)
            if kind == 'captured':
                command += [str(capture), str(shim)]
                env['GPA_LOCAL_INJECT'] = 'true'
            with (directory / (kind + '.log')).open('wb') as stream:
                result = subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=60)
            record = dict(exit_code=result.returncode, command=command,
                          files={p.name: sha(p) for p in output.iterdir() if p.is_file()})
            case['runs'][kind] = record
            save()
            if result.returncode:
                record['failure'] = (output / 'error.txt').read_text() if (output / 'error.txt').exists() else 'See process log/exit code'
                save()
                print(mode, kind, 'failed:', record['failure'], flush=True)
                break
            oracle = json.loads((output / 'oracle.json').read_text())
            require(oracle['completed'] and len(oracle['frames']) == 12, 'Incomplete producer')
            require(all(f['binding_checks'] >= 8 and f['draws_verified'] == len(colors)
                        and f['image_verified'] and f['present_status'] == 0 for f in oracle['frames']),
                    'Incomplete state/draw checks')
            for step, color in enumerate(colors):
                pixel = bytearray([0, 0, 0, 255]); pixel[color] = 255
                golden = bytes(pixel) * 64
                require((output / f'step-{step}.rgba').read_bytes() == golden, 'Step image differs from CPU color oracle')
            require((output / 'frame.rgba').read_bytes() == golden, 'Final image differs from last draw')
        case['accepted_producer'] = len(case['runs']) == 2 and all(r['exit_code'] == 0 for r in case['runs'].values())
        if case['accepted_producer']:
            require(capture.exists() and capture.stat().st_size > 0, 'Missing delivered capture')
            case['capture_sha256'] = sha(capture)
            print('Context-state mode', mode, 'verified', flush=True)
        save()
    manifest['completed'] = True
    manifest['all_producers_verified'] = all(c['accepted_producer'] for c in manifest['cases'])
    save()


if __name__ == '__main__':
    main()
