"""Serial replay oracle for recorded HUD two-swap-chain selection experiments."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('evidence','replay','oracle-tools','qt-bin','out'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    def read(path):return json.loads(path.read_text(encoding='utf-8-sig'))
    checks=[]
    def check(name,passed):
        checks.append(dict(name=name,passed=bool(passed)))
        (out/'validation.json').write_text(json.dumps(dict(passed=all(c['passed'] for c in checks),checks=checks),indent=2))
        if not passed:raise AssertionError(name)
    env=dict(os.environ);env.pop('GPA_LOCAL_INJECT',None);env['PATH']=str(args.qt_bin.resolve())+os.pathsep+env['PATH']
    def run(name,command):
        result=subprocess.run(list(map(str,command)),env=env,capture_output=True,timeout=90)
        (out/(name+'.log')).write_bytes(result.stdout+b'\n'+result.stderr);check(name,result.returncode==0)
    root=args.evidence.resolve();baseline=read(root/'baseline.json')
    swaps=baseline['swap_chains']['active']
    check('two-real-foreground-candidates',len(swaps)==2 and all(s['foreground'] and s['recent_present'] and s['successful_presents']>=2 for s in swaps) and swaps[0]['window']==swaps[1]['window'])
    for name in ('ambiguous','ambiguous-again'):
        status=read(root/(name+'.json'))
        check(name+'-refused',status['hud']['last_request']==dict(ok=False,error='swap_chain_selection_required') and status['hud']['message']=='Select swap chain' and not status['capture_progress']['busy'])
    failed=read(root/'concurrent-refusal.json')
    check('selected-second-was-armed',failed['hud']['last_request']['ok'] and failed['hud']['last_request']['swap_chain']==2)
    check('overlapping-present-explicitly-unsupported',failed['capture_progress']['state']=='Failed' and failed['capture_progress']['error'].startswith('Other swap-chain Present during capture:') and not (root/'session'/failed['capture_progress']['request_name']).exists())
    files=[]
    for name,identity in (('selected-first',1),('selected-second',2),('automatic-first',1)):
        status=read(root/(name+'.json'));capture=status['capture_progress']
        check(name+'-target',capture['state']=='Completed' and capture['swap_chain']==identity and status['hud']['selected_swap_chain']==(0 if name.startswith('automatic') else identity))
        source=Path(capture['file']);files.append(source);data=source.read_bytes()
        check(name+'-header',len(data)>=300 and data[:4]==b'IGPA')
        count=struct.unpack_from('<I',data,12)[0];table=struct.unpack_from('<Q',data,244)[0]
        check(name+'-bounded-index',table<=len(data) and count<=(len(data)-table)//24)
        clears=[];identities=set()
        for i in range(count):
            rid,position,size,flags,category,kind=struct.unpack_from('<QQIBBH',data,table+i*24)
            if rid in identities or flags or position>len(data) or size>len(data)-position:raise ValueError('Invalid capture record')
            identities.add(rid)
            if (category,kind)==(7,0x32):clears.append((position,size))
        check(name+'-one-clear',len(clears)==1 and clears[0][1]==41)
        expected=bytes((255,0,0,255) if identity==1 else (0,0,255,255))*(800*600)
        preview,size=struct.unpack_from('<QI',data,276)
        check(name+'-bounded-preview',preview<=len(data) and size<=len(data)-preview)
        bmp=data[preview:preview+size]
        check(name+'-preview-layout',len(bmp)==54+len(expected) and bmp[:2]==b'BM' and struct.unpack_from('<ii',bmp,18)==(800,600))
        pixels=bytearray(b"".join(bmp[54+y*3200:54+(y+1)*3200] for y in range(599,-1,-1)));pixels[0::4],pixels[2::4]=pixels[2::4],pixels[0::4]
        check(name+'-clean-preview',pixels==expected)
        changed=bytearray(data);struct.pack_into('<4f',changed,clears[0][0]+25,0,1,0,1)
        negative=out/(name+'-green-clear.gpa_frame');negative.write_bytes(changed)
        for label,frame,expected_pixels in (('captured',source,expected),('green-clear',negative,bytes((0,255,0,255))*(800*600))):
            prefix=name+'-'+label
            run(prefix+'-flora',[args.replay,'replay',frame,'--out',out/(prefix+'-flora')])
            run(prefix+'-original',[sys.executable,args.oracle_tools/'replay_frame.py',frame,'--out',out/(prefix+'-original'),'--timeout','60'])
            flora=read(out/(prefix+'-flora/report.json'));original=read(out/(prefix+'-original/replay_report.json'))
            check(prefix+'-exact-dual-execution',(out/(prefix+'-flora/frame.rgba')).read_bytes()==expected_pixels and original['replayed_rgba_sha256']==hashlib.sha256(expected_pixels).hexdigest() and flora['reference_pixels_used'] is False and original['new_framebuffer_count']>0 and original['native']['open_status']==original['native']['playback_status']==0 and original['native']['closed'])
            check(prefix+'-events',flora['counts']=={'ClearRenderTargetView':1,'Present':1})
            check(prefix+'-reference-control',original['comparison_to_captured_resource']['rgba_equal'] is (label=='captured'))
        run(name+'-inventory',[args.replay,'inventory',source,'--out',out/(name+'-inventory')])
        types=[r['type'] for r in read(out/(name+'-inventory/report.json'))['resources']]
        check(name+'-only-application-objects',types.count(0x85)==1 and types.count(0x83)==0 and not any(0x90<=kind<=0x95 for kind in types))
    check('exactly-three-published',set(files)==set((root/'session').glob('*.gpa_frame')) and not list((root/'session').glob('*.partial')))
    end=read(root/'target/exit.json')
    check('native-surface-bytes-retained',end['passed'] and all(s['exact_pixels'] and s['presents']>2 for s in end['surfaces']))
    (out/'inputs.json').write_text(json.dumps({str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in [*files,args.replay,args.oracle_tools/'replay_frame.py']},indent=2))
    print(json.dumps(dict(passed=True,frames=len(files),checks=len(checks))))


if __name__=='__main__':main()
