"""Execute owned-model or wrapped-context MSAA encoder files in both players."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('probe', 'replay', 'oracle-tools', 'qt-bin', 'out'):
        p.add_argument('--'+name, required=True, type=Path)
    p.add_argument('--recorded', action='store_true', help='Validate the wrapped-context recording probe')
    p.add_argument('--rgba16', action='store_true', help='Verify both bytes of recorded RGBA16 components')
    p.add_argument('--depth', action='store_true', help='Verify exact D32S8 words and stencil byte tiles')
    p.add_argument('--queries', action='store_true', help='Validate pre-frame EVENT observations with MSAA initialization')
    p.add_argument('--deferred', action='store_true', help='Validate repeated deferred MSAA executions')
    a = p.parse_args()
    if a.queries and not a.recorded:p.error('--queries requires --recorded')
    if a.rgba16 and not a.recorded:p.error('--rgba16 requires --recorded')
    if a.depth and (not a.recorded or a.rgba16):p.error('--depth requires --recorded and excludes --rgba16')
    if a.deferred and (not a.recorded or a.queries):p.error('--deferred requires --recorded and excludes queries')
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env.pop('GPA_LOCAL_INJECT', None)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env['PATH'] = str(a.qt_bin.resolve())+os.pathsep+env['PATH']
    checks, frames = [], []

    def check(name, ok):
        checks.append(dict(name=name, passed=bool(ok)))
        (out/'validation.json').write_text(json.dumps(dict(scope=__doc__, passed=all(c['passed'] for c in checks),
            frames=frames, checks=checks), indent=2)+'\n')
        if not ok:
            raise AssertionError(name)

    def run(name, command):
        result = subprocess.run([str(x) for x in command], capture_output=True, env=env, timeout=120)
        (out/(name+'.log')).write_bytes(result.stdout+b'\n'+result.stderr)
        check(name, result.returncode == 0)
        return result.stdout

    def parse(path):
        raw = path.read_bytes()
        count = struct.unpack_from('<I', raw, 12)[0]
        offset = struct.unpack_from('<Q', raw, 244)[0]
        entries = [struct.unpack_from('<QQIBBH', raw, offset+24*i) for i in range(count)]
        helpers, annotations = [], []
        active = False
        for ident, at, size, flags, category, kind in entries:
            if category != 7:
                continue
            if kind == 0x327b:
                label = raw[at+24:at+size].decode('utf-16le').rstrip('\0')
                if label.startswith('FloraCapture initialization: MSAA resource '):
                    active = True
                    annotations.append(label)
            elif kind == 0x327c:
                active = False
            elif active and kind == 0x37:
                helpers.append(ident)
        return raw, offset, entries, helpers, annotations

    for device in ('warp', 'hardware'):
        result = json.loads(run(device+'-probe', [a.probe.resolve(), *(['--hardware'] if device == 'hardware' else []), out/device]))
        check(device+'-owned-source-debug-refusals', result['passed'] and result['source_gpu_objects_alive'] == 0 and
              result['debug_warnings_or_errors'] == 0 and
              (result['refusals'] == 8 if a.deferred else result['encoder_refusals'] == 20 if a.queries else result['recording_refusals'] == 4 if a.recorded else result['encoder_refusals'] == 50))
        if a.rgba16 and not a.queries:check(device+'-rgba16-refusals',result['component_bytes']==2 and (result['refusals']==8 if a.deferred else result['encoder_refusals']==6))
        if a.depth and not a.queries:check(device+'-depth-refusals',result['refusals']==8 if a.deferred else result['depth_format']==19 and result['encoder_refusals']==30)
        manifest = json.loads((out/device/'frames.json').read_text())
        for samples in (2, 4):
            source = out/device/f'{samples}x-complete.gpa_frame'
            raw, offset, records, helpers, labels = parse(source)
            check(device+f'-{samples}x-helper-labels', len(labels) == 1 and len(helpers) == samples*2*(9 if a.depth else 1))
            data = bytearray(raw)
            keep = [e for e in records if e[0] not in helpers]
            struct.pack_into('<I', data, 12, len(keep))
            for i, entry in enumerate(keep):
                struct.pack_into('<QQIBBH', data, offset+24*i, *entry)
            name = f'{samples}x-missing-initialization'
            (out/device/(name+'.gpa_frame')).write_bytes(data)
            expected = bytearray()
            for y in range(14):
                for x in range(13*samples*(2 if a.rgba16 or a.depth else 1)):
                    sample=x//(26 if a.rgba16 or a.depth else 13)
                    modified=x%13<6 and sample%2==0
                    color=tuple((word>>(((x//13)%2)*8))&255 for word in (0x3e00,0xbc00,0x0400,0x3555)) if a.rgba16 else (240,19,73,255)
                    if a.depth:
                        color=((0xa6 if modified else 0),0,0,255) if (x//13)%2 else ((0,0,128,62) if modified else (0,0,0,0))
                        expected += bytes(color)
                    else:expected += bytes(color if modified else (0,0,0,0))
            (out/device/(name+'.expected.rgba')).write_bytes(expected)
            manifest.append(dict(file=name+'.gpa_frame', samples=samples, application_draws=(5 if a.depth else 7) if a.deferred else 3, helper_draws=0))
        for item in manifest:
            frame = out/device/item['file']
            case = device+'-'+frame.stem
            if a.queries:
                _, _, event_records, _, _ = parse(frame)
                check(case+'-event-records', sum(e[4] == 7 and e[5] == 0x34fa for e in event_records) == 1 and
                      sum(e[4] == 7 and e[5] == 0x34fb for e in event_records) == 2)
            expected = frame.with_suffix('.expected.rgba').read_bytes()
            frames.append(dict(file=frame.relative_to(out).as_posix(), sha256=hashlib.sha256(frame.read_bytes()).hexdigest(),
                               expected_sha256=hashlib.sha256(expected).hexdigest()))
            run(case+'-flora', [a.replay.resolve(), 'replay', frame, '--out', out/(case+'-flora')])
            run(case+'-original', [sys.executable, a.oracle_tools.resolve()/'replay_frame.py', frame, '--out', out/(case+'-original'), '--timeout', '60'])
            flora = json.loads((out/(case+'-flora/report.json')).read_text())
            original = json.loads((out/(case+'-original/replay_report.json')).read_text())
            check(case+'-exact-cpu-native', (out/(case+'-flora/frame.rgba')).read_bytes() == expected and
                  original['replayed_rgba_sha256'] == hashlib.sha256(expected).hexdigest())
            check(case+'-actual-execution', flora['reference_pixels_used'] is False and original['native']['open_status'] == 0 and
                  original['native']['playback_status'] == 0 and original['native']['closed'] and original['new_framebuffer_count'] > 0)
            check(case+'-preview-cannot-substitute', original['comparison_to_captured_resource']['rgba_equal'] is ('missing-' not in frame.stem))
            draws = item['application_draws']+item['helper_draws']
            check(case+'-draws', flora['counts']['Draw'] == draws)
            run(case+'-inventory', [a.replay.resolve(), 'inventory', frame, '--out', out/(case+'-inventory')])
            inventory = json.loads((out/(case+'-inventory/report.json')).read_text())
            check(case+'-inventory', inventory['draws'] == draws and inventory['dispatches'] == 0)
            shaders = [r for r in inventory['resources'] if 0x90 <= r['type'] <= 0x95]
            check(case+'-application-helper-shaders', len(shaders) == (6 if a.depth or a.deferred else 5))
            for shader in shaders:
                name = case+'-shader-'+shader['id']
                run(name, [a.replay.resolve(), 'shader', frame, '--id', shader['id'], '--out', out/name])
                check(name+'-dxbc', (out/name/'shader.dxbc').read_bytes().startswith(b'DXBC') and (out/name/'shader.asm').stat().st_size > 50)
            check(case+'-independent-modules', not any('intelswtools/gpa/' in m.replace('\\', '/').lower() or
                  Path(m).name.lower().startswith(('python', 'qt5')) for m in flora['loaded_modules']))
    (out/'inputs.json').write_text(json.dumps({str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in
        (a.probe, a.replay, a.oracle_tools/'replay_frame.py', Path(__file__))}, indent=2)+'\n')
    print(json.dumps(dict(passed=True, files=len(frames), checks=len(checks))))


if __name__ == '__main__':
    main()
