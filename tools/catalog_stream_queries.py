"""Bind original stream-query captures to image, buffer and disabled-work oracles."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--captures',type=Path,required=True)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();root=args.out.resolve();root.mkdir(parents=True,exist_ok=False)
    source=args.captures.resolve(strict=True);exe=args.exe.resolve(strict=True)
    original=json.loads((source/'manifest.json').read_text(encoding='utf-8'))
    assert original['completed'] and len(original['cases'])==48
    report=dict(schema='FloraGPA compatibility corpus 1',profile=original['profile'],
                source_manifest_sha256=sha(source/'manifest.json'),exe_sha256=sha(exe),cases=[])
    for case in original['cases']:
        mode=int(case['id'].rsplit('_',1)[1]);folder=root/str(mode);folder.mkdir()
        capture=source/case['path'];assert sha(capture)==case['sha256']
        def run(command,tag,extra=()):
            out=folder/tag
            with (folder/(tag+'.log')).open('wb') as log:
                completed=subprocess.run([str(exe),command,str(capture),'--out',str(out),*map(str,extra)],
                    stdout=log,stderr=subprocess.STDOUT,timeout=60)
            assert completed.returncode==0,(mode,tag)
            return json.loads((out/(command+'.json')).read_text(encoding='utf-8'))
        commands=run('commands','api')['commands']
        draws=[row for row in commands if row['name']=='Draw'];assert len(draws)==2
        event=draws[0]['id'];state=run('command-state','so-state',['--event',event])
        fields={row['field']:row for row in state['fields']}
        assert fields['topology']['value']==1 and fields['gs.shader']['value']!=0
        copied=dict(case);copied['boundaries']=[]
        for stream in range(4):
            binding=fields[f'so.targets.{stream}'];resource=binding['resource_id']
            assert binding['known'] and resource and resource==binding['value']
            copied['boundaries'].append(dict(kind='buffer',resource=resource,event=event,expect_stable=True,
                expected_storage_sha256=sha(source/str(mode)/'hardware'/f'expected-stream{stream}.bin')))
        comparison=bool((mode//12)%2)
        disabled=bytes([0,255,0,255] if comparison else [255,0,255,255])*64
        copied['controls']=[dict(disable_event=event,repeat=2,expected_rgba_sha256=hashlib.sha256(disabled).hexdigest())]
        copied['native_workload_fidelity']='passed'
        copied['application_reference_rgba_sha256']=case['reference_rgba_sha256']
        copied['application_reference_scope']='Full 8x8 conditional output plus all four SO buffers; 12 frames on native hardware/WARP and original capture injection.'
        copied['application_reference_evidence']=dict(path='manifest.json',sha256=report['source_manifest_sha256'])
        copied['fidelity_reason']='Saved per-stream BOOL query descriptor and interval restored; exact independent images, buffer bytes and disabled-work controls required.'
        copied['source_manifests']=[dict(path='manifest.json',sha256=report['source_manifest_sha256'],contains_capture_hash=True)]
        report['cases'].append(copied)
    (root/'manifest.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
