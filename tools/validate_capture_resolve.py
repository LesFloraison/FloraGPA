"""Execute recorded ResolveSubresource files and deletion controls in both players."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('probe', 'replay', 'oracle-tools', 'qt-bin', 'out'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env.pop('GPA_LOCAL_INJECT', None)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env['PATH'] = str(args.qt_bin.resolve())+os.pathsep+env['PATH']
    checks, files = [], []

    def check(name, ok):
        checks.append(dict(name=name, passed=bool(ok)))
        (out/'validation.json').write_text(json.dumps(dict(scope=__doc__, passed=all(c['passed'] for c in checks),
            files=files, checks=checks), indent=2)+'\n')
        if not ok:
            raise AssertionError(name)

    def run(name, command):
        result = subprocess.run([str(x) for x in command], capture_output=True, env=env, timeout=120)
        (out/(name+'.log')).write_bytes(result.stdout+b'\n'+result.stderr)
        check(name, result.returncode == 0)
        return result.stdout

    for device in ('warp', 'hardware'):
        native = json.loads(run(device+'-probe', [args.probe.resolve(), *(['--hardware'] if device == 'hardware' else []), out/device]))
        check(device+'-native-oracle-refusals-lifetime-debug', native['passed'] and native['encoder_refusals'] == 72 and
              native['source_gpu_objects_alive'] == 0 and native['debug_warnings_or_errors'] == 0)
        manifest = json.loads((out/device/'frames.json').read_text())
        check(device+'-profiles', len(manifest) == 24)
        for item in manifest:
            frame = out/device/item['file']
            case = device+'-'+frame.stem
            expected = frame.with_suffix('.expected.rgba').read_bytes()
            raw = frame.read_bytes()
            count = struct.unpack_from('<I', raw, 12)[0]
            offset = struct.unpack_from('<Q', raw, 244)[0]
            resolves = []
            for i in range(count):
                ident, at, size, flags, category, kind = struct.unpack_from('<QQIBBH', raw, offset+24*i)
                if category == 7 and kind == 0x42:
                    check(case+f'-resolve-{ident}-wire-length', size == 44)
                    link, context, dst, sub, src, src_sub, fmt = struct.unpack_from('<QQQIQII', raw, at)
                    resolves.append((link, context, dst, sub, src, src_sub, fmt))
            check(case+'-ordered-resolve-arguments', len(resolves) == item['resolves'] and (not resolves or
                  [r[3:] for r in resolves] == [(1, resolves[0][4], 1, 10 if item['wide'] else 28),
                                               (1, resolves[0][4], 1, 10 if item['wide'] else 28),
                                               (3, resolves[0][4], 0, 10 if item['wide'] else 28)]))
            files.append(dict(file=frame.relative_to(out).as_posix(), sha256=hashlib.sha256(raw).hexdigest(),
                              expected_sha256=hashlib.sha256(expected).hexdigest()))
            run(case+'-flora', [args.replay.resolve(), 'replay', frame, '--out', out/(case+'-flora')])
            run(case+'-original', [sys.executable, args.oracle_tools.resolve()/'replay_frame.py', frame,
                                   '--out', out/(case+'-original'), '--timeout', '60'])
            flora = json.loads((out/(case+'-flora/report.json')).read_text())
            original = json.loads((out/(case+'-original/replay_report.json')).read_text())
            check(case+'-exact-cpu-native', (out/(case+'-flora/frame.rgba')).read_bytes() == expected and
                  original['replayed_rgba_sha256'] == hashlib.sha256(expected).hexdigest())
            check(case+'-actual-execution', flora['reference_pixels_used'] is False and original['native']['open_status'] == 0 and
                  original['native']['playback_status'] == 0 and original['native']['closed'] and original['new_framebuffer_count'] > 0)
            check(case+'-deletion-changes-result-with-same-reference', original['comparison_to_captured_resource']['rgba_equal'] is
                  ('missing-' not in frame.stem))
            check(case+'-executed-events', flora['counts']['Draw'] == item['draws'] and flora['counts'].get('ResolveSubresource', 0) == item['resolves'])
            run(case+'-inventory', [args.replay.resolve(), 'inventory', frame, '--out', out/(case+'-inventory')])
            inventory = json.loads((out/(case+'-inventory/report.json')).read_text())
            check(case+'-event-inventory', inventory['draws'] == item['draws'])
            shaders = [r for r in inventory['resources'] if 0x90 <= r['type'] <= 0x95]
            check(case+'-shader-inventory', len(shaders) == 5)
            for shader in shaders:
                name = case+'-shader-'+shader['id']
                run(name, [args.replay.resolve(), 'shader', frame, '--id', shader['id'], '--out', out/name])
                check(name+'-dxbc', (out/name/'shader.dxbc').read_bytes().startswith(b'DXBC') and (out/name/'shader.asm').stat().st_size > 50)
            check(case+'-independent-modules', not any('intelswtools/gpa/' in m.replace('\\', '/').lower() or
                  Path(m).name.lower().startswith(('python', 'qt5')) for m in flora['loaded_modules']))
    paths = (args.probe, args.replay, args.oracle_tools/'replay_frame.py', Path(__file__))
    (out/'inputs.json').write_text(json.dumps({str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}, indent=2)+'\n')
    print(json.dumps(dict(passed=True, files=len(files), checks=len(checks))))


if __name__ == '__main__':
    main()
