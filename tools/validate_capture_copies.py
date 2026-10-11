"""Serial dual-player checks for independently captured whole-resource copies."""
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
    checks = []

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        (out/'validation.json').write_text(json.dumps(dict(
            passed=all(c['passed'] for c in checks), scope=__doc__, checks=checks), indent=2)+'\n')
        if not passed:
            raise AssertionError(name)

    def run(name, command):
        result = subprocess.run([str(v) for v in command], env=env, capture_output=True, timeout=90)
        (out/(name+'.log')).write_bytes(result.stdout+b'\n'+result.stderr)
        check(name, result.returncode == 0)

    for mode in ('buffer', 'texture', 'buffer-negative', 'texture-negative'):
        frame = out/(mode+'.gpa_frame')
        negative, texture = mode.endswith('negative'), mode.startswith('texture')
        run(mode+'-probe', [args.probe.resolve(), frame, mode])
        raw = frame.read_bytes()
        count = struct.unpack_from('<I', raw, 12)[0]
        table = struct.unpack_from('<Q', raw, 244)[0]
        check(mode+'-index', table <= len(raw) and count <= (len(raw)-table)//24)
        entries = {}
        for i in range(count):
            identity, offset, size, _, category, kind = struct.unpack_from('<QQIBBH', raw, table+i*24)
            if identity in entries or offset > len(raw) or size > len(raw)-offset:
                raise ValueError('Invalid copy container index')
            entries[identity] = (category, kind, raw[offset:offset+size])
        copies = [p for c, t, p in entries.values() if c == 7 and t == 0x3e]
        check(mode+'-copy-count', len(copies) == (0 if negative else 2))
        for i, payload in enumerate(copies):
            link, context, destination, source = struct.unpack('<QQQQ', payload)
            check(mode+'-copy-refs-'+str(i), link == 0 and context == 1 and destination != source
                  and entries[destination][:2] == entries[source][:2] == (5, 0x85 if texture else 0x83))
        run(mode+'-flora', [args.replay.resolve(), 'replay', frame, '--out', out/(mode+'-flora')])
        run(mode+'-original', [sys.executable, args.oracle_tools.resolve()/'replay_frame.py',
                              frame, '--out', out/(mode+'-original'), '--timeout', '60'])
        expected = b''.join(bytes((0, 0, 255, 255) if negative else
                                 (0, 255, 0, 255) if x < 4 else (255, 0, 0, 255))
                            for y in range(8) for x in range(8))
        native = json.loads((out/(mode+'-flora/report.json')).read_text())
        original = json.loads((out/(mode+'-original/replay_report.json')).read_text())
        check(mode+'-exact-cpu', (out/(mode+'-flora/frame.rgba')).read_bytes() == expected
              and original['replayed_rgba_sha256'] == hashlib.sha256(expected).hexdigest())
        check(mode+'-execution', native['reference_pixels_used'] is False
              and original['new_framebuffer_count'] > 0 and original['native']['open_status'] == 0
              and original['native']['playback_status'] == 0 and original['native']['closed'])
        check(mode+'-reference-control', original['comparison_to_captured_resource']['rgba_equal'] is (not negative))
        counts = dict(Draw=2, Present=1)
        if not negative:
            counts['CopyResource'] = 2
        counts.update(dict(ClearRenderTargetView=1) if texture else dict(Map=1, unmap_observations=1))
        check(mode+'-events', native['counts'] == counts)
        run(mode+'-inventory', [args.replay.resolve(), 'inventory', frame, '--out', out/(mode+'-inventory')])
        inventory = json.loads((out/(mode+'-inventory/report.json')).read_text())
        check(mode+'-draws', inventory['draws'] == 2)
        for shader in (r for r in inventory['resources'] if 0x90 <= r['type'] <= 0x95):
            name = mode+'-shader-'+shader['id']
            run(name, [args.replay.resolve(), 'shader', frame, '--id', shader['id'], '--out', out/name])
            check(name+'-dxbc', (out/name/'shader.dxbc').read_bytes().startswith(b'DXBC')
                  and (out/name/'shader.asm').stat().st_size > 50)
        check(mode+'-lifetimes', json.loads((out/(mode+'-probe.log')).read_text())['source_gpu_objects_alive'] == 0)
    evidence = {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in (args.probe, args.replay, args.oracle_tools/'replay_frame.py')}
    evidence['frames'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in out.glob('*.gpa_frame')}
    (out/'inputs.json').write_text(json.dumps(evidence, indent=2)+'\n')
    print(json.dumps(dict(passed=True, checks=len(checks))))


if __name__ == '__main__':
    main()
