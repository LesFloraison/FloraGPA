"""Development-only original GPA buffer creation fixtures; never rewrites captures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
from capture_present import digest, require


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--producer', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--gpa-dir', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    args = parser.parse_args()
    producer = args.producer.resolve(strict=True)
    gpa = args.gpa_dir.resolve(strict=True)
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).parent
    manifest = {'schema':'FloraGPA compatibility corpus 1', 'profile':'GPA 2025 R1 legacy DX11 / IGPA v3',
                'root_parameter':'--captures-root', 'completed':False, 'cases':[], 'producer_runs':[],
                'producer_sha256':digest(producer),
                'sources':{name:digest(source/name) for name in ['native/buffer_creation_probe.cpp',
                    'native/present_capture_probe.cpp', 'capture_buffers.py', 'capture_present.py']},
                'gpa_sha256':{name:digest(gpa/name) for name in ['shimloader64.dll','shimd3d64.dll']}}

    def save():
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')

    try:
        for mode, name in enumerate(['initial','upload','validation','failed','initial_then_update']):
            folder=out/str(mode); folder.mkdir(); capture=folder/'capture.gpa_frame'
            for backend in ['native','captured']:
                env=dict(os.environ); env.pop('GPA_LOCAL_INJECT',None)
                command=[str(producer),str(folder/backend),str(mode)]
                if backend=='captured':
                    command.extend([str(capture),str(gpa/'shimloader64.dll')]);env['GPA_LOCAL_INJECT']='true'
                with (folder/(backend+'.log')).open('wb') as log:
                    process=subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT,
                                           creationflags=subprocess.CREATE_NO_WINDOW,timeout=60)
                record={'mode':mode,'backend':backend,'exit_code':process.returncode}
                manifest['producer_runs'].append(record);save()
                require(process.returncode==0,f'{mode}/{backend}: producer failed; inspect raw logs')
                oracle=json.loads((folder/backend/'oracle.json').read_text());record['oracle']=oracle
                require(oracle['completed'] and len(oracle['frames'])==12,'Producer incomplete')
                require(oracle['capture_requested']==(backend=='captured'),'Unexpected capture injection state')
                require((folder/backend/'frame.rgba').read_bytes()==bytes([0,255,0,255])*64,
                        'Native buffer-dependent GPU image mismatch')
                result=1 if mode==2 else -2147024809 if mode==3 else 0
                require(all(f['hresult']==result and f['buffer_created']==(mode not in (2,3))
                            for f in oracle['frames']),'CreateBuffer result mismatch')
                if mode not in (2,3):
                    expected=struct.pack('<16I',*[((0x11110000 if mode==0 else 0x22220000)+i) for i in range(16)])
                    require((folder/backend/'buffer.bin').read_bytes()==expected,'Buffer CPU oracle mismatch')
                save()
            manifest['cases'].append({'id':'buffer_'+name,'path':f'{mode}/capture.gpa_frame',
                'sha256':digest(capture),'bytes':capture.stat().st_size,'family':'buffer_creation',
                'origin':'self_owned_original_gpa_capture','source_manifests':[],
                'provenance_note':'Unmodified CaptureNextFrame output; buffer bytes additionally checked by native producer and C++ boundary tests.',
                'device_scope':'Default hardware producer; original player adapter unidentified.',
                'reference_rgba_sha256':hashlib.sha256(bytes([0,255,0,255])*64).hexdigest(),
                'comparison_policy':'exact_golden','boundaries':[],'controls':[]})
            save();print(name,'captured',flush=True)
        manifest['completed']=True
    except Exception as error:
        manifest['error']=str(error);raise
    finally:
        save()


if __name__=='__main__':
    main()
