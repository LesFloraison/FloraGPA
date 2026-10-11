"""Serial dual-player checks for independently captured subresource region copies."""
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

    for mode in (str(d)+e+n for d in range(1,5) for e in ('','-extended') for n in ('','-negative')):
        frame = out/(mode+'.gpa_frame')
        negative, dim, extended = mode.endswith('negative'), int(mode[0]), 'extended' in mode
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
        copies = [p for c, t, p in entries.values() if c == 7 and t == (0x256 if extended else 0x40)]
        check(mode+'-copy-count', len(copies) == (0 if negative else 2))
        for i, payload in enumerate(copies):
            link, context, destination, sub, x, y, z, source, src_sub, has_box = struct.unpack_from('<QQQIIIIQIB', payload)
            check(mode+'-copy-refs-'+str(i), link==0 and context==1 and destination!=source and sub==src_sub==0
                  and entries[destination][:2]==entries[source][:2]==(5,0x82+dim))
            origin=(16 if i==0 else 32,0,0) if dim==1 else (2 if i==0 else 3,(1 if i==0 else 2) if dim>=3 else 0,(1 if i==0 else 2) if dim==4 else 0)
            check(mode+'-owned-region-'+str(i), (x,y,z)==origin and has_box==i and len(payload)==53+24*i+(4 if extended else 0))
            if i:
                check(mode+'-box', struct.unpack_from('<6I',payload,53)==((16,0,0,48,1,1) if dim==1 else (1,1 if dim>=3 else 0,0,3,3 if dim>=3 else 1,1)))
            if extended:check(mode+'-flags-'+str(i), struct.unpack_from('<I',payload,len(payload)-4)[0]==0)
        run(mode+'-flora', [args.replay.resolve(), 'replay', frame, '--out', out/(mode+'-flora')])
        run(mode+'-original', [sys.executable, args.oracle_tools.resolve()/'replay_frame.py',
                              frame, '--out', out/(mode+'-original'), '--timeout', '60'])
        values=[1]*(64 if dim==1 else 8 if dim==2 else 64 if dim==3 else 256)
        if not negative:
            if dim==1:
                values[4:36]=[2]*32;values[8:16]=[3]*8
            else:
                for z in range(1 if dim==4 else 0,3 if dim==4 else 1):
                    for y in range(1 if dim>=3 else 0,5 if dim>=3 else 1):
                        for x in range(2,6):values[z*64+y*8+x]=2
                for z in range(2 if dim==4 else 0,3 if dim==4 else 1):
                    for y in range(2 if dim>=3 else 0,4 if dim>=3 else 1):
                        for x in range(3,5):values[z*64+y*8+x]=3
        expected=b''.join(bytes({1:(255,255,0,255),2:(0,255,0,255),3:(255,0,0,255)}[values[x if dim==2 else (1+(x&1))*64+y*8+x if dim==4 else y*8+x]]) for y in range(8) for x in range(8))
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
            counts['CopySubresourceRegion1' if extended else 'CopySubresourceRegion'] = 2
        counts['UpdateSubresource']=1
        check(mode+'-pinned-player',original['native']['player_sha256']=='39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b')
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
