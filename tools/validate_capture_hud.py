"""Development-only, serial dual-player oracle for FloraCaptureHudLiveTarget files."""
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
    for name in ('captures', 'replay', 'oracle-tools', 'qt-bin', 'out'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--flip', action='store_true', help='Expect the native flip Present RTV-unbinding diagnostic')
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
        (out/'validation.json').write_text(json.dumps(dict(passed=all(c['passed'] for c in checks), checks=checks), indent=2))
        if not passed:
            raise AssertionError(name)

    def run(name, command):
        result = subprocess.run([str(x) for x in command], capture_output=True, env=env, timeout=90)
        (out/(name+'.log')).write_bytes(result.stdout+b'\n'+result.stderr)
        check(name, result.returncode == 0)

    expected = bytes((32, 48, 96, 255))*(800*600-1)+bytes((255, 255, 255, 255))
    negative_expected = expected[:-4]+bytes((0, 0, 0, 255))
    files = sorted(args.captures.resolve().glob('*.gpa_frame'))
    check('captures-exist', bool(files))
    for index, source in enumerate(files):
        raw = source.read_bytes()
        check(f'{index}-header', len(raw) >= 300 and raw[:4] == b'IGPA')
        count = struct.unpack_from('<I', raw, 12)[0]
        table = struct.unpack_from('<Q', raw, 244)[0]
        check(f'{index}-bounded-index', table <= len(raw) and count <= (len(raw)-table)//24)
        entries = {}
        for i in range(count):
            identity, position, size, flags, category, kind = struct.unpack_from('<QQIBBH', raw, table+i*24)
            if identity in entries or flags or position > len(raw) or size > len(raw)-position:
                raise ValueError('Malformed capture fixture')
            entries[identity] = (position, size, category, kind)
        draws = [(position, size) for position, size, category, kind in entries.values() if (category, kind) == (7, 0x37)]
        check(f'{index}-one-app-draw', len(draws) == 1 and draws[0][1] == 32)
        preview, size = struct.unpack_from('<QI', raw, 276)
        check(f'{index}-bounded-preview', preview <= len(raw) and size <= len(raw)-preview)
        bmp = raw[preview:preview+size]
        check(f'{index}-preview-layout', len(bmp) == 54+len(expected) and bmp[:2] == b'BM' and struct.unpack_from('<ii', bmp, 18) == (800, 600))
        rgba = bytearray(b"".join(bmp[54+y*3200:54+(y+1)*3200] for y in range(599,-1,-1)));rgba[0::4], rgba[2::4] = rgba[2::4], rgba[0::4]
        check(f'{index}-clean-preview', rgba == expected)
        negative = bytearray(raw)
        struct.pack_into('<I', negative, draws[0][0]+24, 0)
        negative_path = out/f'{index}-draw-zero.gpa_frame'
        negative_path.write_bytes(negative)
        for label, frame, pixels in (('captured', source, expected), ('draw-zero', negative_path, negative_expected)):
            name = f'{index}-{label}'
            run(name+'-flora', [args.replay.resolve(), 'replay', frame, '--out', out/(name+'-flora')])
            run(name+'-original', [sys.executable, args.oracle_tools.resolve()/'replay_frame.py', frame, '--out', out/(name+'-original'), '--timeout', '60'])
            flora = json.loads((out/(name+'-flora/report.json')).read_text())
            original = json.loads((out/(name+'-original/replay_report.json')).read_text())
            check(name+'-exact-native-pixels', (out/(name+'-flora/frame.rgba')).read_bytes() == pixels)
            check(name+'-exact-original-pixels', original['replayed_rgba_sha256'] == hashlib.sha256(pixels).hexdigest())
            check(name+'-executed', flora['reference_pixels_used'] is False and original['new_framebuffer_count'] > 0 and original['native']['open_status'] == 0 and original['native']['playback_status'] == 0 and original['native']['closed'])
            check(name+'-reference-control', original['comparison_to_captured_resource']['rgba_equal'] is (label == 'captured'))
            expected_counts = {'UpdateSubresource': 1, 'Draw': 1, 'Present': 1}
            if args.flip:
                expected_counts['present_rtv_unbinds'] = 1
            check(name+'-no-hud-commands', flora['counts'] == expected_counts)
        name = f'{index}-inventory'
        run(name, [args.replay.resolve(), 'inventory', source, '--out', out/name])
        inventory = json.loads((out/name/'report.json').read_text())
        types = [r['type'] for r in inventory['resources']]
        check(name+'-only-app-texture', types.count(0x85) == 1 and types.count(0x83) == 0)
        check(name+'-only-app-shaders', sum(0x90 <= kind <= 0x95 for kind in types) == 2)
        for shader in (r for r in inventory['resources'] if 0x90 <= r['type'] <= 0x95):
            shader_name = f'{index}-shader-{shader["id"]}'
            run(shader_name, [args.replay.resolve(), 'shader', source, '--id', shader['id'], '--out', out/shader_name])
            check(shader_name+'-dxbc', (out/shader_name/'shader.dxbc').read_bytes().startswith(b'DXBC'))
    inputs = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in [args.replay.resolve(), args.oracle_tools.resolve()/'replay_frame.py', *files]}
    (out/'inputs.json').write_text(json.dumps(inputs, indent=2))
    print(json.dumps(dict(passed=True, frames=len(files), checks=len(checks))))


if __name__ == '__main__':
    main()
