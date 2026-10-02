"""Development-only original GPA texture copy/resolve fixtures; never rewrites captures."""
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
                'sources':{name:digest(source/name) for name in ['native/texture_copy_probe.cpp',
                    'native/present_capture_probe.cpp', 'capture_texture_copies.py', 'capture_present.py']},
                'gpa_sha256':{name:digest(gpa/name) for name in ['shimloader64.dll','shimd3d64.dll']}}
    def save():
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')

    try:
        for mode, name in enumerate(['whole_2d','array_mip_region','same_resource_layers','volume_region','one_d_region','bc_mip_edge','depth_whole','msaa_copy','resolve_typed','resolve_typeless','bc_mip_null']):
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
                        'Native texture-dependent GPU image mismatch')
                require(all(f['bytes_verified'] and f['image_verified'] for f in oracle['frames']),'Copy producer status mismatch')
                expected=(folder/backend/'expected.bin').read_bytes()
                require((folder/backend/'texture.bin').read_bytes()==expected,'Texture byte oracle mismatch')
                if backend=='captured':
                    require(expected==(folder/'native'/'expected.bin').read_bytes(),'Native/shim CPU oracle differs')
                save()
            manifest['cases'].append({'id':'texture_copy_'+name,'path':f'{mode}/capture.gpa_frame',
                'sha256':digest(capture),'bytes':capture.stat().st_size,'family':'texture_copy',
                'origin':'self_owned_original_gpa_capture','source_manifests':[],
                'provenance_note':'Unmodified CaptureNextFrame output; texture bytes additionally checked by native producer and C++ boundary tests.',
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
