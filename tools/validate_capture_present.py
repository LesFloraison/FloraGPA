"""Serial dual-player gate for native Present-boundary transactions (internal control API)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import sys


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for option in ('probe','replay','oracle-tools','qt-bin','out'):
        p.add_argument('--'+option,type=Path,required=True)
    p.add_argument('--hud',action='store_true',help='Validate 800x600 HUD publication/cancellation captures')
    p.add_argument('--flip-mode',type=int,choices=(1,2))
    a=p.parse_args()
    if a.hud and not a.flip_mode:p.error('--hud requires --flip-mode')
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    env=dict(os.environ);env.pop('GPA_LOCAL_INJECT',None);env['PYTHONDONTWRITEBYTECODE']='1'
    env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env['PATH'];checks=[]
    def check(name,ok):
        checks.append(dict(name=name,passed=bool(ok)))
        (out/'validation.json').write_text(json.dumps(dict(passed=all(x['passed'] for x in checks),scope=__doc__,checks=checks),indent=2))
        if not ok:raise AssertionError(name)
    def run(name,command):
        r=subprocess.run([str(x) for x in command],env=env,capture_output=True,timeout=90)
        (out/(name+'.log')).write_bytes(r.stdout+b'\n'+r.stderr);check(name,r.returncode==0)
    command=[a.probe.resolve(),out/'native']
    if a.flip_mode:command.append(a.flip_mode)
    if a.hud:command.append('--hud')
    run('native-transactions',command)
    expected=bytes((255,0,0,255))*64
    if a.hud:
        row=bytes((255,0,0,255))*8+bytes((0,0,0,255))*792
        expected=row*8+bytes((0,0,0,255))*(800*592)
    files=sorted((out/'native').glob('*.gpa_frame'))
    check('published-controls-exist',all((out/'native'/f'frame-{i}.gpa_frame') in files for i in range(3)))
    for index,frame in enumerate(files):
        variants=[('captured',frame,expected)]
        if a.hud:
            raw=frame.read_bytes();count=struct.unpack_from('<I',raw,12)[0];table=struct.unpack_from('<Q',raw,244)[0]
            check(f'{index}-bounded-index',table<=len(raw) and count<=(len(raw)-table)//24)
            draws=[]
            for i in range(count):
                identity,position,size,flags,category,kind=struct.unpack_from('<QQIBBH',raw,table+i*24)
                check(f'{index}-record-{i}',not flags and position<=len(raw) and size<=len(raw)-position)
                if (category,kind)==(7,0x37):draws.append((position,size))
            check(f'{index}-app-draw',len(draws)==1 and draws[0][1]==32)
            preview,size=struct.unpack_from('<QI',raw,276);check(f'{index}-bounded-preview',preview<=len(raw) and size<=len(raw)-preview)
            bmp=raw[preview:preview+size];check(f'{index}-preview-layout',len(bmp)==54+len(expected) and bmp[:2]==b'BM' and struct.unpack_from('<ii',bmp,18)==(800,600))
            rgba=bytearray(b"".join(bmp[54+y*3200:54+(y+1)*3200] for y in range(599,-1,-1)));rgba[0::4],rgba[2::4]=rgba[2::4],rgba[0::4];check(f'{index}-clean-preview',rgba==expected)
            negative=bytearray(raw);struct.pack_into('<I',negative,draws[0][0]+24,0);path=out/f'{index}-draw-zero.gpa_frame';path.write_bytes(negative)
            variants.append(('draw-zero',path,bytes((0,0,0,255))*(800*600)))
        for label,source,pixels in variants:
            name=f'{index}-{label}'
            run(name+'-flora',[a.replay.resolve(),'replay',source,'--out',out/(name+'-flora')])
            run(name+'-original',[sys.executable,a.oracle_tools.resolve()/'replay_frame.py',source,'--out',out/(name+'-original'),'--timeout','60'])
            flora=json.loads((out/(name+'-flora/report.json')).read_text());original=json.loads((out/(name+'-original/replay_report.json')).read_text())
            check(name+'-both-players-exact-cpu',(out/(name+'-flora/frame.rgba')).read_bytes()==pixels and original['replayed_rgba_sha256']==hashlib.sha256(pixels).hexdigest())
            counts={'ClearRenderTargetView':1,'Draw':1,'Present':1}
            if a.flip_mode:counts['present_rtv_unbinds']=1
            check(name+'-recorded-events',flora['counts']==counts)
            check(name+'-native-execution',flora['reference_pixels_used'] is False)
            check(name+'-original-execution',original['native']['open_status']==0 and original['native']['playback_status']==0 and original['native']['closed'] and original['new_framebuffer_count']>0)
            check(name+'-actual-captured-backbuffer',original['comparison_to_captured_resource']['rgba_equal'] is (label=='captured'))
        if a.hud:
            name=f'{index}-inventory';run(name,[a.replay.resolve(),'inventory',frame,'--out',out/name]);inventory=json.loads((out/name/'report.json').read_text());types=[r['type'] for r in inventory['resources']]
            check(name+'-only-application-gpu-objects',types.count(0x85)==1 and types.count(0x83)==0 and sum(0x90<=k<=0x95 for k in types)==2)
    evidence={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in (a.probe,a.replay,a.oracle_tools/'replay_frame.py')}
    evidence['frames']={path.name:hashlib.sha256(path.read_bytes()).hexdigest() for path in (out/'native').glob('*.gpa_frame')}
    (out/'inputs.json').write_text(json.dumps(evidence,indent=2));print(json.dumps(dict(passed=True,checks=len(checks))))


if __name__=='__main__':main()
