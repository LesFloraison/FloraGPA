"""Development-only serial original captures; native and injected failures stay separate."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(__doc__)
    for name in ['exe', 'shim', 'out']:
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--materialize-baseline', action='store_true')
    args = parser.parse_args()
    exe, shim, root = args.exe.resolve(strict=True), args.shim.resolve(strict=True), args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    frozen = root/'producer'
    frozen.mkdir()
    repo = Path(__file__).resolve().parents[1]
    for source in ['tools/capture_deferred_versions.py', 'tools/native/deferred_version_probe.cpp',
                   'tools/native/original_capture_control.h', 'tools/native/texture_probe_helpers.h',
                   'tools/native/present_capture_probe.cpp']:
        shutil.copy2(repo/source, frozen/Path(source).name)
    shutil.copy2(exe, frozen/exe.name)
    manifest = dict(schema='FloraGPA deferred version producer evidence 1', completed=False,
                    materialize_baseline=args.materialize_baseline,
                    producer_files={p.name:sha(p) for p in frozen.iterdir()},
                    original_dlls={name:sha(shim.parent/name) for name in [shim.name, 'shimd3d64.dll']},
                    cases=[])

    def save():
        (root/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')

    for mode in range(9):
        directory = root/str(mode)
        directory.mkdir()
        capture = directory/'capture.gpa_frame'
        case = dict(mode=mode, runs={})
        manifest['cases'].append(case)
        for kind in ['native', 'captured']:
            output = directory/kind
            command = [str(exe), str(output), str(mode + (9 if args.materialize_baseline else 0))]
            env = os.environ.copy()
            env.pop('GPA_LOCAL_INJECT', None)
            if kind == 'captured':
                command += [str(capture), str(shim)]
                env['GPA_LOCAL_INJECT'] = 'true'
            with (directory/(kind+'.log')).open('wb') as log:
                try:
                    result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=60)
                    record = dict(exit_code=result.returncode, timed_out=False)
                except subprocess.TimeoutExpired:
                    record = dict(exit_code=None, timed_out=True)
            record.update(command=command, files={p.name:sha(p) for p in output.glob('*') if p.is_file()})
            case['runs'][kind] = record
            try:
                oracle = json.loads((output/'oracle.json').read_text())
                assert oracle['completed'] and len(oracle['frames']) == 12
                errors = []
                for frame in oracle['frames']:
                    if args.materialize_baseline:
                        assert (output/f"frame-{frame['frame']}-baseline.bin").read_bytes() == struct.pack('<4I',101,103,107,109)
                    assert len(frame['steps']) == 3
                    for step, expected in enumerate([(3,1,2,3), (22,2,5,5), (78,3,11,3)]):
                        raw = (output/f"frame-{frame['frame']}-step-{step}.bin").read_bytes()
                        actual = struct.unpack('<4I', raw)
                        assert list(actual) == frame['steps'][step]['actual']
                        assert list(expected) == frame['steps'][step]['expected']
                        if actual != expected:
                            errors.append(dict(frame=frame['frame'], step=step, actual=actual, expected=expected))
                    rgba = (output/f"frame-{frame['frame']}.rgba").read_bytes()
                    assert (rgba == bytes([0,255,0,255])*64) == frame['image_verified']
                record.update(completed=True, storage_errors=errors, image_failures=oracle['image_failures'],
                              finish_state_failures=oracle['finish_state_failures'],
                              execute_state_failures=oracle['execute_state_failures'])
                if kind == 'native':
                    assert record['exit_code'] == 0 and not errors and not oracle['image_failures']
                    assert not oracle['finish_state_failures'] and not oracle['execute_state_failures']
                if kind == 'captured' and capture.is_file():
                    case.update(capture_sha256=sha(capture), capture_bytes=capture.stat().st_size)
            except Exception as error:
                record['validation_error'] = str(error) or type(error).__name__
            save()
            print(mode, kind, {k:v for k,v in record.items() if k not in ['command','files','storage_errors']}, flush=True)
            if kind == 'native' and record.get('validation_error'):
                break
        save()
    manifest['completed'] = True
    manifest['native_baselines_passed'] = all(c['runs']['native'].get('completed') and
                                            not c['runs']['native'].get('validation_error') for c in manifest['cases'])
    manifest['delivered_captures'] = sum('capture_sha256' in c for c in manifest['cases'])
    manifest['captured_data_checks_passed'] = all(
        c['runs'].get('captured', {}).get('completed') and
        c['runs']['captured'].get('exit_code') == 0 and
        not c['runs']['captured'].get('validation_error') and
        not c['runs']['captured'].get('storage_errors') and
        not c['runs']['captured'].get('image_failures') for c in manifest['cases'])
    save()
    return 0 if (manifest['native_baselines_passed'] and manifest['delivered_captures']==9
                 and manifest['captured_data_checks_passed']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
