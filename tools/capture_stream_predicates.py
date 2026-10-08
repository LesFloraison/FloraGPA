"""Capture real multistream SO overflow workloads; GPA is development-only."""
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


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--producer', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--gpa-dir', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--modes', type=int, nargs='+', default=list(range(48)))
    args = parser.parse_args()
    if len(set(args.modes)) != len(args.modes) or any(n < 0 or n >= 48 for n in args.modes):
        parser.error('Choose distinct modes 0..47')
    root = args.out.resolve(); root.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[1]
    frozen = root / 'producer'; frozen.mkdir()
    for name in ['tools/native/stream_predicate_probe.cpp', 'tools/native/texture_probe_helpers.h',
                 'tools/native/present_capture_probe.cpp', 'tools/native/original_capture_control.h',
                 'tools/capture_stream_predicates.py']:
        shutil.copy2(repo / name, frozen / Path(name).name)
    shutil.copy2(args.producer.resolve(strict=True), frozen / args.producer.name)
    manifest = dict(schema='FloraGPA compatibility corpus 1', profile='GPA 2025 R1 legacy DX11 / IGPA v3',
                    completed=False, producer_files={p.name: sha(p) for p in frozen.iterdir()},
                    gpa_sha256={n: sha(args.gpa_dir/n) for n in ['shimloader64.dll', 'shimd3d64.dll', 'dx11_player.dll']},
                    cases=[], producer_runs=[])
    def save():
        (root/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    try:
        for mode in args.modes:
            folder=root/str(mode); folder.mkdir(); capture=folder/'capture.gpa_frame'
            stream,scenario,comparison=mode%4,(mode//4)%3,bool((mode//12)%2)
            overflow=-1 if scenario==0 else (stream+(scenario==2))%4
            predicate=overflow>=0
            expected=bytes([0,255,0,255] if predicate!=comparison else [255,0,255,255])*64
            buffers=[b''.join(struct.pack('<4f',s+1,v+10,20,30) for v in range(1 if s==overflow else 2)) for s in range(4)]
            for backend in ['hardware','warp','captured']:
                output=folder/backend; command=[str(frozen/args.producer.name),str(output),str(mode)]
                env=dict(os.environ)
                for name in ['GPA_LOCAL_INJECT','FLORA_STREAM_PREDICATE_WARP','FLORA_STREAM_PREDICATE_DEBUG']: env.pop(name,None)
                if backend=='warp': env['FLORA_STREAM_PREDICATE_WARP']='1'
                if backend!='captured': env['FLORA_STREAM_PREDICATE_DEBUG']='1'
                else:
                    env['GPA_LOCAL_INJECT']='true'; command += [str(capture),str(args.gpa_dir/'shimloader64.dll')]
                row=dict(mode=mode,backend=backend,command=command);manifest['producer_runs'].append(row);save()
                with (folder/(backend+'.log')).open('wb') as log:
                    result=subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT,
                                          creationflags=subprocess.CREATE_NO_WINDOW,timeout=90)
                row['exit_code']=result.returncode;save()
                assert result.returncode==0, f'Producer failed: {mode}/{backend}'
                oracle=json.loads((output/'oracle.json').read_text());row['oracle']=oracle
                assert oracle['completed'] and len(oracle['frames'])==12 and oracle['query_type']==9+stream*2
                assert oracle['warp']==(backend=='warp') and oracle['debug']==(backend!='captured')
                assert oracle['capture_requested']==(backend=='captured') and oracle['frame_creation']==(mode>=24)
                assert oracle['overflow_stream']==overflow
                if backend!='captured': assert (output/'debug.txt').read_bytes()==b''
                for i,frame in enumerate(oracle['frames']):
                    assert frame==dict(frame=i,query_value=scenario==1,predicate_value=predicate,comparison=comparison,image_verified=True,buffers_verified=4)
                    assert (output/f'frame{i}.rgba').read_bytes()==expected
                    for s in range(4): assert (output/f'frame{i}-stream{s}.bin').read_bytes()==buffers[s]
                assert (output/'expected.rgba').read_bytes()==expected
                for s in range(4): assert (output/f'expected-stream{s}.bin').read_bytes()==buffers[s]
                row['evidence_files']={p.name:sha(p) for p in sorted(output.iterdir()) if p.is_file()}
                save()
            manifest['cases'].append(dict(id=f'stream_predicate_{mode}',path=f'{mode}/capture.gpa_frame',
                sha256=sha(capture),bytes=capture.stat().st_size,family='stream_predicate',
                origin='self_owned_original_gpa_capture',source_manifests=[],
                provenance_note='Unmodified original CaptureNextFrame; native hardware/WARP debug and CPU image/buffer oracles frozen.',
                device_scope='Default hardware capture, native hardware/WARP verified; original player adapter not matched.',
                reference_rgba_sha256=hashlib.sha256(expected).hexdigest(),comparison_policy='exact_golden',boundaries=[],controls=[],
                covers=[f'Query stream {stream}',f'overflow stream {overflow}',f'comparison {comparison}',
                        'frame-created' if mode>=24 else 'preframe-created refreshed interval']))
            save()
        manifest['completed']=True
    finally: save()


if __name__=='__main__':main()
