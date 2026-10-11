"""Serial validation of pre-frame EVENT observations, not in-frame GPU waits."""
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
    for key in ('probe', 'replay', 'oracle-tools', 'qt-bin', 'out'):
        parser.add_argument('--'+key, type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env.pop('GPA_LOCAL_INJECT', None)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env['PATH'] = str(args.qt_bin.resolve())+os.pathsep+env['PATH']
    checks = []

    def check(name, value):
        checks.append(dict(name=name, passed=bool(value)))
        (out/'validation.json').write_text(json.dumps(dict(
            passed=all(c['passed'] for c in checks), scope=__doc__, checks=checks), indent=2)+'\n')
        if not value:
            raise AssertionError(name)

    def run(name, command):
        p = subprocess.run([str(v) for v in command], capture_output=True, env=env, timeout=90)
        (out/(name+'.log')).write_bytes(p.stdout+b'\n'+p.stderr)
        check(name, p.returncode == 0)

    for mode in ('preframe','preframe1','preframe-negative','preframe1-negative'):
        negative = mode.endswith('negative')
        frame = out/(mode+'.gpa_frame')
        run(mode+'-probe', [args.probe.resolve(), frame, mode])
        raw = frame.read_bytes()
        count, = struct.unpack_from('<I', raw, 12)
        table, = struct.unpack_from('<Q', raw, 244)
        entries = {}
        for i in range(count):
            identity, offset, size, _, category, kind = struct.unpack_from('<QQIBBH',raw,table+i*24)
            if identity in entries or offset>len(raw) or size>len(raw)-offset:
                raise ValueError('Invalid EVENT frame index')
            entries[identity]=(category,kind,raw[offset:offset+size])
        desc = [p for c,t,p in entries.values() if c==7 and t==0x3152]
        end = [p for c,t,p in entries.values() if c==7 and t==0x34fa]
        get = [p for c,t,p in entries.values() if c==7 and t==0x34fb]
        check(mode+'-query-records', len(desc)==len(end)==len(get)==1)
        link, query, present, kind, flags = struct.unpack('<QQBII', desc[0])
        check(mode+'-ordinary-descriptor', (link,present,kind,flags)==(0,1,0,0) and query not in entries)
        check(mode+'-end-identity', struct.unpack('<QQQ',end[0])==(0,1,query))
        check(mode+'-captured-result', struct.unpack('<QQiQBIII',get[0])==(0,1,0,query,1,1,4,0))
        run(mode+'-validate', [args.replay.resolve(),'validate-frame',frame,'--out',out/(mode+'-validate')])
        validation=json.loads((out/(mode+'-validate/validation.json')).read_text())
        check(mode+'-validated-observation', validation['errors']==0 and any(f['kind']=='ordinary_event_end_observation' for f in validation['findings']) and any(f['kind']=='query_completion_not_saved' for f in validation['findings']))
        run(mode+'-flora', [args.replay.resolve(),'replay',frame,'--out',out/(mode+'-flora')])
        run(mode+'-original', [sys.executable,args.oracle_tools.resolve()/'replay_frame.py',frame,'--out',out/(mode+'-original'),'--timeout','60'])
        native=json.loads((out/(mode+'-flora/report.json')).read_text())
        original=json.loads((out/(mode+'-original/replay_report.json')).read_text())
        expected=bytes((0,0,255,255) if negative else (0,255,0,255))*16
        check(mode+'-exact-cpu', (out/(mode+'-flora/frame.rgba')).read_bytes()==expected and original['replayed_rgba_sha256']==hashlib.sha256(expected).hexdigest())
        check(mode+'-execution', native['reference_pixels_used'] is False and original['new_framebuffer_count']>0 and original['native']['open_status']==original['native']['playback_status']==0 and original['native']['closed'])
        check(mode+'-reference-control', original['comparison_to_captured_resource']['rgba_equal'] is (not negative))
        check(mode+'-player-version', original['native']['player_sha256']=='39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b')
        counts=dict(CopyResource=1,Present=1,query_metadata_records=2,query_end_observations=1,query_completion_unresolved=1)
        if not negative:counts['ClearRenderTargetView']=1
        check(mode+'-events-no-invented-wait', native['counts']==counts)
        check(mode+'-lifetimes', json.loads((out/(mode+'-probe.log')).read_text())['source_gpu_objects_alive']==0)
    evidence={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (args.probe,args.replay,args.oracle_tools/'replay_frame.py',Path(__file__))}
    evidence['frames']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.glob('*.gpa_frame')}
    (out/'inputs.json').write_text(json.dumps(evidence,indent=2)+'\n')
    print(json.dumps(dict(passed=True,checks=len(checks))))


if __name__ == '__main__':
    main()
