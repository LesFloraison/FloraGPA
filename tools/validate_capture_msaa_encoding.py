"""Serial MSAA initialization wire experiment and production scissor encoder regression.

MSAA files use the test-only format writer. No production MSAA/GF2 acceptance.
"""
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
    for key in ('msaa-probe', 'replay', 'oracle-tools', 'qt-bin', 'out'):
        parser.add_argument('--'+key, type=Path, required=True)
    parser.add_argument('--scissor-probe', type=Path)
    parser.add_argument('--depth', action='store_true', help='Validate the isolated D32S8 wire experiment')
    args = parser.parse_args()
    if not args.depth and not args.scissor_probe:
        parser.error('--scissor-probe is required for the color/scissor regression')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env.pop('GPA_LOCAL_INJECT', None)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env['PATH'] = str(args.qt_bin.resolve())+os.pathsep+env['PATH']
    checks, files = [], []

    def check(name, ok):
        checks.append(dict(name=name, passed=bool(ok)))
        (out/'validation.json').write_text(json.dumps(dict(
            scope=__doc__, passed=all(c['passed'] for c in checks), files=files, checks=checks), indent=2)+'\n')
        if not ok:
            raise AssertionError(name)

    def run(name, command):
        result = subprocess.run([str(x) for x in command], capture_output=True, env=env, timeout=120)
        (out/(name+'.log')).write_bytes(result.stdout+b'\n'+result.stderr)
        check(name, result.returncode == 0)
        return result.stdout

    def entries(frame):
        data = frame.read_bytes()
        count = struct.unpack_from('<I', data, 12)[0]
        offset = struct.unpack_from('<Q', data, 244)[0]
        return [(ident, category, kind, data[at:at+size]) for ident, at, size, flags, category, kind in
                (struct.unpack_from('<QQIBBH', data, offset+24*i) for i in range(count))]

    for device in ('warp', 'hardware'):
        probes = [('msaa', args.msaa_probe)]
        if args.scissor_probe:
            probes.append(('scissor', args.scissor_probe))
        for fixture, probe in probes:
            name = device+'-'+fixture
            probe_result = json.loads(run(name+'-probe', [probe.resolve(),
                *(['--hardware'] if device == 'hardware' else []), out/name]))
            check(name+'-native-oracle-lifetime-debug', probe_result['source_gpu_objects_alive'] == 0 and
                  probe_result['debug_warnings_or_errors'] == 0 and probe_result['passed'])
            manifest = json.loads((out/name/'frames.json').read_text()) if fixture == 'msaa' else None
            frames = sorted((out/name).glob('*.gpa_frame'))
            check(name+'-file-count', len(frames) == (6 if fixture == 'msaa' else 4))
            for frame in frames:
                case = name+'-'+frame.stem
                expected = frame.with_suffix('.expected.rgba').read_bytes()
                files.append(dict(file=frame.relative_to(out).as_posix(),
                                  sha256=hashlib.sha256(frame.read_bytes()).hexdigest(),
                                  expected_sha256=hashlib.sha256(expected).hexdigest()))
                run(case+'-flora', [args.replay.resolve(), 'replay', frame, '--out', out/(case+'-flora')])
                run(case+'-original', [sys.executable, args.oracle_tools.resolve()/'replay_frame.py', frame,
                                      '--out', out/(case+'-original'), '--timeout', '60'])
                flora = json.loads((out/(case+'-flora/report.json')).read_text())
                original = json.loads((out/(case+'-original/replay_report.json')).read_text())
                check(case+'-both-exact-native-cpu', (out/(case+'-flora/frame.rgba')).read_bytes() == expected and
                      original['replayed_rgba_sha256'] == hashlib.sha256(expected).hexdigest())
                check(case+'-actual-gpu-execution', flora['reference_pixels_used'] is False and
                      original['native']['open_status'] == 0 and original['native']['playback_status'] == 0 and
                      original['native']['closed'] and original['new_framebuffer_count'] > 0)
                check(case+'-captured-reference-negative', original['comparison_to_captured_resource']['rgba_equal'] is
                      ('missing-' not in frame.stem))
                records = entries(frame)
                check(case+'-scissor-mask-enabled', all(struct.unpack_from('<I', payload, 84)[0] & 1
                      for _, category, kind, payload in records if category == 3 and kind == 3))
                draws = 2
                if manifest:
                    item = next(m for m in manifest if m['file'] == frame.name)
                    draws = len(item['application_draws'])+len(item['helper_events'])-1
                    check(case+'-explicit-helper-provenance', not set(item['application_draws']) & set(item['helper_events']) and
                          len(item['application_draws']) == (3 if item['application_edits'] else 1))
                    source = next(p for ident, category, kind, p in records if ident == item['msaa_resource'])
                    check(case+'-real-msaa-array', struct.unpack_from('<I', source, 36)[0] == item['samples'] and
                          struct.unpack_from('<I', source, 28)[0] == item['layers'])
                    if args.depth:
                        check(case+'-d32s8-initialization-draws', struct.unpack_from('<I', source, 32)[0] == 19 and
                              len(item['helper_events']) == (9*item['samples']*item['layers'] if item['initialization'] else 0)+1)
                    label = next(p for _, category, kind, p in records if category == 7 and kind == 0x327b)
                    check(case+'-initialization-labelled', 'FloraCapture initialization (experimental)' in label[24:].decode('utf-16le'))
                check(case+'-executed-draw-count', flora['counts']['Draw'] == draws)
                run(case+'-inventory', [args.replay.resolve(), 'inventory', frame, '--out', out/(case+'-inventory')])
                inventory = json.loads((out/(case+'-inventory/report.json')).read_text())
                check(case+'-event-inventory', inventory['draws'] == draws and inventory['dispatches'] == 0)
                shaders = [r for r in inventory['resources'] if 0x90 <= r['type'] <= 0x95]
                check(case+'-shader-inventory', len(shaders) == ((5 if args.depth else 4) if manifest else 3))
                for shader in shaders:
                    prefix = case+'-shader-'+shader['id']
                    run(prefix, [args.replay.resolve(), 'shader', frame, '--id', shader['id'], '--out', out/prefix])
                    check(prefix+'-dxbc-disassembly', (out/prefix/'shader.dxbc').read_bytes().startswith(b'DXBC') and
                          (out/prefix/'shader.asm').stat().st_size > 50)
                check(case+'-flora-independent-modules', not any('intelswtools/gpa/' in m.replace('\\', '/').lower()
                      or Path(m).name.lower().startswith(('python', 'qt5')) for m in flora['loaded_modules']))
    inputs = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
              (args.msaa_probe, args.scissor_probe, args.replay, args.oracle_tools/'replay_frame.py', Path(__file__)) if p}
    (out/'inputs.json').write_text(json.dumps(inputs, indent=2)+'\n')
    print(json.dumps(dict(passed=True, files=len(files), checks=len(checks))))


if __name__ == '__main__':
    main()
