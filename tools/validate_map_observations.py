"""Development-only three-way Map validation: producer, original captures, native replay bytes."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from catalog_captures import digest


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--research-root', type=Path, required=True)
    p.add_argument('--exe', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    root, exe, out = a.research_root.resolve(), a.exe.resolve(strict=True), a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    samples = root / 'analysis/capture_samples/texture_maps'
    manifest = json.loads((samples / 'manifest.json').read_text())
    for name, expected in manifest['sources'].items():
        if digest(root / name) != expected:
            raise RuntimeError('Producer source changed since capture: ' + name)
    hidden = subprocess.STARTUPINFO()
    hidden.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    hidden.wShowWindow = 0
    def run(cmd, log, env=None):
        with log.open('wb') as f:
            r = subprocess.run(list(map(str, cmd)), stdout=f, stderr=subprocess.STDOUT,
                               env=env, startupinfo=hidden, creationflags=subprocess.CREATE_NO_WINDOW, timeout=180)
        if r.returncode:
            raise RuntimeError(f'Child failed ({r.returncode}): {log}')
    run([sys.executable, root / 'tools/build_deferred_probe.py', '--source',
         root / 'tools/native/texture_map_capture_probe.cpp', '--out', out / 'producer'], out / 'build.log')
    system_env = {k:v for k,v in os.environ.items() if k.upper() in
                  {'SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}}
    system_env['PATH'] = str(Path(os.environ['WINDIR']) / 'System32')
    checks = []
    for case in manifest['cases']:
        capture = samples / case['file']
        if digest(capture) != case['sha256']:
            raise RuntimeError('Capture hash changed: ' + str(capture))
        directory = out / case['name']; directory.mkdir()
        run([out / 'producer/FloraGpaDeferredProbe.exe', directory / 'producer', case['mode']],
            directory / 'producer.log', system_env)
        oracle = json.loads((directory / 'producer/oracle.json').read_text())
        assert oracle['frames'] == 12 and oracle['failures'] == oracle['reset_failures'] == 0
        assert not oracle['shim_present'] and not oracle['capture_requested']
        assert digest(directory / 'producer/texture.bin') == case['texture_sha256']
        assert digest(directory / 'producer/oracle.rgba') == case['rgba_sha256']
        last = case['maps'][-1]; info = last['resource_info']
        for driver in ('hardware', 'warp'):
            for before in (True, False):
                tag = driver + ('-before' if before else '-after')
                target = directory / tag
                cmd = [exe, 'texture', capture, '--id', last['resource'], '--event', last['id'],
                       '--mip', last['subresource'] % info['mips'], '--layer', last['subresource'] // info['mips'],
                       '--out', target]
                if driver == 'warp': cmd += ['--warp']
                if before: cmd += ['--before']
                run(cmd, directory / (tag + '.log'), system_env)
                actual = (target / 'subresource.bin').read_bytes()
                expected = (directory / 'producer/texture.bin').read_bytes()
                if before: expected = bytes(len(expected))
                assert actual == expected, (case['name'], tag)
                report = json.loads((target / 'report.json').read_text())
                checks.append({'case': case['name'], 'driver': driver, 'before': before,
                               'capture_sha256': case['sha256'], 'sha256': digest(target / 'subresource.bin'),
                               'bytes': len(actual), 'passed': True, 'counts': report['counts']})
                (out / 'validation.json').write_text(json.dumps({'checks': checks}, indent=2) + '\n')
                print(case['name'], tag, 'PASS', flush=True)
    (out / 'validation.json').write_text(json.dumps({'passed': True, 'checks': checks,
        'exe_sha256': digest(exe), 'manifest_sha256': digest(samples / 'manifest.json'),
        'producer_build': json.loads((out / 'producer/build.json').read_text())}, indent=2) + '\n')


if __name__ == '__main__':
    main()
