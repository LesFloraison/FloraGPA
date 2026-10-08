"""Generate original writable DO_NOT_WAIT captures and independent byte oracles."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def writes(path):
    raw = path.read_bytes()
    assert raw[:4] == b'IGPA' and raw[0x44:0x48] == b'DX11'
    count = struct.unpack_from('<I', raw, 12)[0]
    table = struct.unpack_from('<Q', raw, 0xf4)[0]
    assert table <= len(raw) and count <= (len(raw)-table)//24
    entries = {}
    for i in range(count):
        identity, offset, size, _, category, kind = struct.unpack_from('<QQIBBH', raw, table+i*24)
        assert identity not in entries and offset <= len(raw) and size <= len(raw)-offset
        entries[identity] = category, kind, raw[offset:offset+size]
    result = []
    for identity, (category, kind, body) in sorted(entries.items()):
        if category != 7 or kind != 0x246:
            continue
        assert len(body) == 48
        link, context, hr, resource, sub, mode, flags, data = struct.unpack('<QQiQIIIQ', body)
        assert not link and not sub and mode in (2, 3) and flags == 0x100000
        record = dict(event=identity,context=context,result=hr,resource=resource,kind=mode,flags=flags,data_id=data)
        if hr == 0:
            dc, dt, payload = entries[data]
            assert dc == 9 and dt in (1, 0x100)
            record.update(data_type=dt,data_sha256=hashlib.sha256(payload).hexdigest())
            if dt == 1:
                assert len(payload) >= 4 and struct.unpack_from('<I',payload)[0] == len(payload)-4
                record['storage_sha256'] = hashlib.sha256(payload[4:]).hexdigest()
        result.append(record)
    assert len([r for r in result if r['result']==0]) == 1
    return result


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--producer',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--gpa-dir',type=Path,default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--modes',type=int,nargs='+',default=list(range(8)))
    args=parser.parse_args()
    assert len(set(args.modes))==len(args.modes) and all(0<=m<8 for m in args.modes)
    root=args.out.resolve();root.mkdir(parents=True,exist_ok=False)
    frozen=root/'producer';frozen.mkdir()
    repo=Path(__file__).resolve().parents[1]
    for name in ['tools/native/map_nowait_probe.cpp','tools/native/texture_probe_helpers.h',
                 'tools/native/present_capture_probe.cpp','tools/native/original_capture_control.h',
                 'tools/capture_map_nowait.py']:
        shutil.copy2(repo/name,frozen/Path(name).name)
    shutil.copy2(args.producer,frozen/args.producer.name)
    manifest=dict(schema='FloraGPA writable NOWAIT discovery 1',completed=False,producer_files={p.name:sha(p) for p in frozen.iterdir()},
                  gpa_sha256={n:sha(args.gpa_dir/n) for n in ['shimloader64.dll','shimd3d64.dll','dx11_player.dll']},producer_runs=[],cases=[])
    def save():(root/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    try:
        for mode in args.modes:
            folder=root/str(mode);folder.mkdir();capture=folder/'capture.gpa_frame'
            dimension=mode%4;width=[65536,8192,1025,65][dimension];height=[1,1,127,31][dimension];depth=7 if dimension==3 else 1
            count=width*height*depth
            initial=bytes((i*13+17)%251 for i in range(count*4))
            expected=bytearray(initial if mode>=4 else bytes((i*29+73)%251 for i in range(count*4)))
            if mode>=4:
                for n,pixel in enumerate([0,width-1,count//2,count-1]):expected[pixel*4:pixel*4+4]=bytes(203+n*7+c for c in range(4))
            for backend in ['hardware','warp','captured']:
                output=folder/backend;command=[str(frozen/args.producer.name),str(output),str(mode)]
                env=dict(os.environ)
                for key in ['GPA_LOCAL_INJECT','FLORA_MAP_NOWAIT_WARP','FLORA_MAP_NOWAIT_DEBUG']:env.pop(key,None)
                if backend=='warp':env['FLORA_MAP_NOWAIT_WARP']='1'
                if backend!='captured':env['FLORA_MAP_NOWAIT_DEBUG']='1'
                else:env['GPA_LOCAL_INJECT']='true';command += [str(capture),str(args.gpa_dir/'shimloader64.dll')]
                row=dict(mode=mode,backend=backend,command=command);manifest['producer_runs'].append(row);save()
                with (folder/(backend+'.log')).open('wb') as log:
                    result=subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=90,creationflags=subprocess.CREATE_NO_WINDOW)
                row['exit_code']=result.returncode;save();assert result.returncode==0,(mode,backend)
                oracle=json.loads((output/'oracle.json').read_text());row['oracle']=oracle
                assert oracle['completed'] and len(oracle['frames'])==12 and oracle['mode']==mode
                assert oracle['warp']==(backend=='warp') and oracle['capture_requested']==(backend=='captured')
                assert oracle['debug']==(backend!='captured')
                if backend!='captured':assert (output/'debug.txt').read_bytes()==b''
                assert (output/'before.bin').read_bytes()==initial and (output/'expected.bin').read_bytes()==expected
                for frame in range(12):assert (output/f'frame{frame}.bin').read_bytes()==expected
                row['evidence_files']={p.name:sha(p) for p in output.iterdir() if p.is_file()};save()
            recorded=writes(capture)
            for record in recorded:
                if record['result']==0 and record['data_type']==1:assert record['storage_sha256']==hashlib.sha256(expected).hexdigest()
            manifest['cases'].append(dict(mode=mode,path=f'{mode}/capture.gpa_frame',sha256=sha(capture),bytes=capture.stat().st_size,
                                         writes=recorded,before_sha256=hashlib.sha256(initial).hexdigest(),after_sha256=hashlib.sha256(expected).hexdigest()))
            save();print(mode,'captured',len(recorded),'write attempts',flush=True)
        manifest['completed']=True
    finally:save()


if __name__=='__main__':main()
