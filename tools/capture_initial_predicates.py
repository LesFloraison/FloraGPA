"""Serial, development-only original captures of missing and refreshed predicate history."""
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
    for name in ('producer', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--gpa-dir', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    args = parser.parse_args()
    exe = args.producer.resolve(strict=True)
    gpa = args.gpa_dir.resolve(strict=True)
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    frozen = root/'producer'
    frozen.mkdir()
    repo = Path(__file__).resolve().parents[1]
    for name in ('tools/capture_initial_predicates.py', 'tools/native/initial_predicate_probe.cpp',
                 'tools/native/original_capture_control.h', 'tools/native/texture_probe_helpers.h',
                 'tools/native/present_capture_probe.cpp'):
        shutil.copy2(repo/name, frozen/Path(name).name)
    shutil.copy2(exe, frozen/exe.name)
    manifest = dict(schema='FloraGPA compatibility corpus 1',
                    profile='GPA 2025 R1 legacy DX11 / IGPA v3', completed=False,
                    producer_files={p.name: sha(p) for p in frozen.iterdir()},
                    gpa_sha256={n: sha(gpa/n) for n in ('shimloader64.dll', 'shimd3d64.dll', 'dx11_player.dll')},
                    cases=[], producer_runs=[])
    hidden = subprocess.STARTUPINFO()
    hidden.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    hidden.wShowWindow = 0

    def save():
        (root/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')

    try:
        for mode in range(16):
            folder = root/str(mode)
            folder.mkdir()
            capture = folder/'capture.gpa_frame'
            visible, comparison, refresh = bool(mode & 1), bool(mode & 2), mode // 4 == 1
            expected = bytes([0, 255, 0, 255] if visible != comparison else [255, 0, 255, 255])*64
            for backend in ('native', 'captured'):
                command = [str(frozen/exe.name), str(folder/backend), str(mode)]
                env = dict(os.environ)
                env.pop('GPA_LOCAL_INJECT', None)
                if backend == 'captured':
                    command.extend([str(capture), str(gpa/'shimloader64.dll')])
                    env['GPA_LOCAL_INJECT'] = 'true'
                record = dict(mode=mode, backend=backend, command=command)
                manifest['producer_runs'].append(record)
                with (folder/(backend+'.log')).open('wb') as log:
                    try:
                        run = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                             startupinfo=hidden, creationflags=subprocess.CREATE_NO_WINDOW,
                                             timeout=60)
                        record['exit_code'] = run.returncode
                    except subprocess.TimeoutExpired:
                        record['timed_out'] = True
                        raise
                save()
                if run.returncode:
                    raise RuntimeError(f'{mode}/{backend}: producer failed; inspect error.txt')
                oracle = json.loads((folder/backend/'oracle.json').read_text())
                record['oracle'] = oracle
                assert oracle['completed'] and len(oracle['frames']) == 12
                for frame in oracle['frames']:
                    assert frame['image_verified'] and frame['query_value'] == visible
                    assert frame['comparison'] == comparison
                    assert frame['interval_issued'] == (frame['frame'] == 0 or refresh)
                    assert frame['query_read'] == (frame['interval_issued'] or mode // 4 == 2)
                assert (folder/backend/'expected.rgba').read_bytes() == expected
                assert (folder/backend/'frame.rgba').read_bytes() == expected
                save()
            manifest['cases'].append(dict(
                id=f'initial_predicate_{mode}', path=f'{mode}/capture.gpa_frame',
                sha256=sha(capture), bytes=capture.stat().st_size, family='initial_predicate',
                origin='self_owned_original_gpa_capture',
                provenance_note='Unmodified CaptureNextFrame output; frozen producer and native/injected oracles retained.',
                source_manifests=[], device_scope='Default hardware producer; original player device not identified.',
                reference_rgba_sha256=hashlib.sha256(expected).hexdigest(),
                comparison_policy='exact_golden', boundaries=[], controls=[],
                covers=['captured Begin/End refresh' if refresh else 'saved GetData observation' if mode // 4 == 2 else
                        'first use before later Begin/End' if mode // 4 == 3 else 'preframe-only predicate result',
                        f'query={visible}, comparison={comparison}']))
            save()
            print(mode, 'captured and producer-verified', flush=True)
        manifest['completed'] = True
    except Exception as error:
        manifest['error'] = str(error) or type(error).__name__
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
