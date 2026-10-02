"""Development-only original GPA Present fixtures from the self-owned C++ producer.

Build FloraPresentCaptureProbe explicitly. This tool never edits a captured file;
use validate_corpus.py with the resulting manifest for independent/player runs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(value, message):
    if not value:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--producer', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--gpa-dir', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    args = parser.parse_args()
    producer = args.producer.resolve(strict=True)
    gpa = args.gpa_dir.resolve(strict=True)
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    hidden = subprocess.STARTUPINFO()
    hidden.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    hidden.wShowWindow = 0
    manifest = {
        'schema': 'FloraGPA compatibility corpus 1',
        'profile': 'GPA 2025 R1 legacy DX11 / IGPA v3',
        'root_parameter': '--captures-root', 'completed': False, 'cases': [], 'producer_runs': [],
        'producer_sha256': digest(producer),
        'source_sha256': digest(Path(__file__).parent/'native/present_capture_probe.cpp'),
        'tool_sha256': digest(Path(__file__)),
        'gpa_sha256': {n: digest(gpa/n) for n in ('shimloader64.dll', 'shimd3d64.dll')},
    }

    def save():
        (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')

    try:
        for mode, name in enumerate(('flip_test', 'secondary_flip', 'secondary_discard',
                                     'secondary_flip_discard', 'secondary_tearing')):
            folder = output/str(mode)
            folder.mkdir()
            capture = folder/'capture.gpa_frame'
            for backend in ('native', 'captured'):
                command = [str(producer), str(folder/backend), str(mode)]
                env = dict(os.environ)
                env.pop('GPA_LOCAL_INJECT', None)
                if backend == 'captured':
                    command.extend([str(capture), str(gpa/'shimloader64.dll')])
                    env['GPA_LOCAL_INJECT'] = 'true'
                with (folder/(backend+'.log')).open('wb') as log:
                    run = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                         startupinfo=hidden, creationflags=subprocess.CREATE_NO_WINDOW,
                                         timeout=60)
                record = {'mode': mode, 'backend': backend, 'exit_code': run.returncode}
                manifest['producer_runs'].append(record)
                save()
                if run.returncode:
                    raise RuntimeError(f'Producer failed: {mode}/{backend}; inspect error.txt')
                record['oracle'] = json.loads((folder/backend/'oracle.json').read_text())
                oracle = record['oracle']
                require(oracle['completed'] and len(oracle['frames']) == 12 and oracle['pixel_failures'] == 0,
                        f'{mode}/{backend}: producer frame or pixel mismatch')
                require(oracle['capture_requested'] == (backend == 'captured'), 'Unexpected injection state')
                require((folder/backend/'oracle.rgba').read_bytes() == bytes([0, 255, 0, 255])*64,
                        'Main buffer CPU oracle mismatch')
                require(all(f['hresult'] == 0 and f['terminal_hresult'] == 0 and f['before']
                            and f['after'] == (mode in (0, 2)) for f in oracle['frames']),
                        f'{mode}/{backend}: native binding oracle mismatch')
                save()
            # The selected secondary Present ends modes 1..4's capture on the red buffer.
            expected = bytes([0, 255, 0, 255] if mode == 0 else [255, 0, 0, 255])*64
            manifest['cases'].append({
                'id': 'present_'+name, 'path': f'{mode}/capture.gpa_frame',
                'sha256': digest(capture), 'bytes': capture.stat().st_size,
                'family': 'present', 'origin': 'self_owned_original_gpa_capture',
                'provenance_note': 'Original CaptureNextFrame output, never rewritten; see producer_runs.',
                'source_manifests': [], 'device_scope': 'Producer uses default hardware adapter; original player adapter unidentified.',
                'reference_rgba_sha256': hashlib.sha256(expected).hexdigest(),
                'comparison_policy': 'exact_golden', 'boundaries': [], 'controls': [],
                'covers': ['Present TEST with subsequent clear/copy' if mode == 0 else
                           'terminal discard binding retention' if mode == 2 else 'terminal flip RTV unbind'],
            })
            save()
            print(name, 'captured', flush=True)
        manifest['completed'] = True
    except Exception as error:
        manifest['error'] = str(error)
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
