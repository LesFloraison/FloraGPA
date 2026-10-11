"""Serial dual-player checks for independently captured DX11 updates."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


PLAYER_SHA256 = '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b'


def region(base, second):
    if base == 'bc':
        return (4,4 if second else 0,0,8,8,1)
    if base == 'buffer':
        return (84,0,0,172,1,1) if second else (36,0,0,220,1,1)
    if base == 'line':
        return (3,0,0,5,1,1) if second else (1,0,0,7,1,1)
    return (3,3,2 if base == 'volume' else 0,5,5,3 if base == 'volume' else 1) if second else (1,1,1 if base == 'volume' else 0,7,7,3 if base == 'volume' else 1)


def boxed_pixel(base, x, y, negative):
    value = 1
    point = (x,y,0) if base == 'bc' else (4*(x+y*8),0,0) if base == 'buffer' else (x,0,0) if base == 'line' else (x,y,1+(x&1) if base == 'volume' else 0)
    if not negative:
        for i in range(2):
            box = region(base, i)
            if all(box[a] <= point[a] < box[a+3] for a in range(3)):
                value = i+2
    return ((255,255,0,255),(0,255,0,255),(255,0,0,255))[value-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('probe', 'replay', 'oracle-tools', 'qt-bin', 'out'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--boxed', action='store_true', help='Use the boxed Update fixture and independent region oracle')
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
            passed=all(c['passed'] for c in checks), scope=__doc__, boxed=args.boxed, checks=checks), indent=2)+'\n')
        if not passed:
            raise AssertionError(name)

    def run(name, command):
        result = subprocess.run([str(v) for v in command], env=env, capture_output=True, timeout=90)
        (out/(name+'.log')).write_bytes(result.stdout+b'\n'+result.stderr)
        check(name, result.returncode == 0)

    bases = ('buffer','line','texture','volume','bc') if args.boxed else ('buffer','line','texture','volume')
    for mode in [base+api+suffix for base in bases for api in ('','1') for suffix in ('','-negative')]:
        frame = out/(mode+'.gpa_frame')
        negative, texture = mode.endswith('negative'), not mode.startswith('buffer')
        base = next(base for base in bases if mode.startswith(base))
        run(mode+'-probe', [args.probe.resolve(), frame, mode])
        raw = frame.read_bytes()
        count = struct.unpack_from('<I', raw, 12)[0]
        table = struct.unpack_from('<Q', raw, 244)[0]
        check(mode+'-index', table <= len(raw) and count <= (len(raw)-table)//24)
        entries = {}
        for i in range(count):
            identity, offset, size, _, category, kind = struct.unpack_from('<QQIBBH', raw, table+i*24)
            if identity in entries or offset > len(raw) or size > len(raw)-offset:
                raise ValueError('Invalid update container index')
            entries[identity] = (category, kind, raw[offset:offset+size])
        updates = [(t,p) for c,t,p in entries.values() if c == 7 and t in (0x247,0x255)]
        check(mode+'-update-count', len(updates) == (0 if negative else 2))
        for i,(kind,payload) in enumerate(updates):
            extended='1' in mode
            link,context,destination,sub,boxed=struct.unpack_from('<QQQIB',payload)
            data_offset = 53 if args.boxed else 29
            data,row,depth=struct.unpack_from('<QII',payload,data_offset)
            if args.boxed:
                box = region(base, i)
                check(mode+'-box-'+str(i), struct.unpack_from('<6I',payload,29) == box)
                row_bytes = (box[3]-box[0])*(1 if base == 'buffer' else 4)
                expected_pitch = (24,64) if base == 'bc' else (row_bytes+16,(row_bytes+16)*(box[4]-box[1])+16)
            else:
                expected_pitch = (40,352) if texture else (0,0)
            check(mode+'-update-layout-'+str(i), kind==(0x255 if extended else 0x247) and len(payload)==data_offset+16+(4 if extended else 0) and boxed==int(args.boxed) and not link and context==1 and sub==(3 if base=='bc' else 0) and entries[destination][:2]==(5,0x84 if base=='line' else 0x86 if base=='volume' else 0x85 if texture else 0x83) and entries[data][:2]==(9,1) and (row,depth)==expected_pitch and (not extended or struct.unpack_from('<I',payload,data_offset+16)[0]==0))
        run(mode+'-flora', [args.replay.resolve(), 'replay', frame, '--out', out/(mode+'-flora')])
        run(mode+'-original', [sys.executable, args.oracle_tools.resolve()/'replay_frame.py',
                              frame, '--out', out/(mode+'-original'), '--timeout', '60'])
        expected = b''.join(bytes(boxed_pixel(base,x,y,negative) if args.boxed else (255, 255, 0, 255) if negative else
                                 (0, 255, 0, 255) if x < 4 else (255, 0, 0, 255))
                            for y in range(8) for x in range(8))
        native = json.loads((out/(mode+'-flora/report.json')).read_text())
        original = json.loads((out/(mode+'-original/replay_report.json')).read_text())
        check(mode+'-player-version', original['native']['player_sha256'] == PLAYER_SHA256)
        check(mode+'-exact-cpu', (out/(mode+'-flora/frame.rgba')).read_bytes() == expected
              and original['replayed_rgba_sha256'] == hashlib.sha256(expected).hexdigest())
        check(mode+'-execution', native['reference_pixels_used'] is False
              and original['new_framebuffer_count'] > 0 and original['native']['open_status'] == 0
              and original['native']['playback_status'] == 0 and original['native']['closed'])
        check(mode+'-reference-control', original['comparison_to_captured_resource']['rgba_equal'] is (not negative))
        counts = dict(Draw=2, Present=1)
        if not negative:
            counts['UpdateSubresource1' if '1' in mode else 'UpdateSubresource'] = 2
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
