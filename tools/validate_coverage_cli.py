"""Check native coverage CLI/worker transport, exports and rejected requests."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--package', type=Path, required=True)
    p.add_argument('--capture', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--qt-bin', type=Path)
    args = p.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = {k:v for k,v in os.environ.items() if k.upper() in
           {'SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}}
    windows = Path(os.environ['SystemRoot'])
    env['PATH'] = os.pathsep.join(([str(args.qt_bin)] if args.qt_bin else []) + [str(windows/'System32'),str(windows)])
    cases = [
        ('fragment',[],True), ('geometry',['--coverage-mode','geometry'],True),
        ('depth',['--coverage-target','depth','--warp'],True),
        ('rt3',['--coverage-target','rt3'],True),
        ('ignore-depth',['--ignore-depth'],True),
        ('disabled',['--disable','100'],True),
        ('unknown-mode',['--coverage-mode','bad'],False),
        ('unknown-target',['--coverage-target','present'],False),
        ('unbound-target',['--coverage-target','rt1'],False),
        ('invalid-layer',['--coverage-layer','1'],False),
        ('negative-layer',['--coverage-layer','-1'],False),
        ('overflow-layer',['--coverage-layer','4294967296'],False),
        ('before',['--before'],False), ('timings',['--timings'],False),
        ('event',['--event','100'],False), ('suppress',['--suppress-draws'],False),
    ]
    checks=[]
    for name,flags,success in cases:
        directory = out/name
        executable = args.package.resolve()/('FloraGPA.Worker.exe' if name == 'geometry' else 'FloraGPA.Cli.exe')
        command=[str(executable),'coverage',str(args.capture.resolve()),'--id','100','--out',str(directory),*flags]
        run=subprocess.run(command,env=env,capture_output=True,timeout=180)
        (out/(name+'.log')).write_bytes(run.stdout+run.stderr)
        assert (run.returncode==0)==success, (name,run.returncode,run.stdout,run.stderr)
        if success:
            coverage=json.loads((directory/'coverage.json').read_text('utf-8'))
            report=json.loads((directory/'report.json').read_text('utf-8'))
            assert report['completed'] and report['coverage']==coverage
            assert coverage['event']['id']==100
            assert coverage['original_submissions']==(0 if name=='disabled' else 1)
            assert coverage['covered_pixels']==0 if name=='disabled' else coverage['covered_pixels']>0
            for filename in ('coverage.png','overlay.png','after_draw.png'):
                raw=(directory/filename).read_bytes()
                assert raw[:8]==b'\x89PNG\r\n\x1a\n'
                assert struct.unpack_from('>2I',raw,16)==(10,7)
            modules=[str(v).lower() for v in report['loaded_modules']]
            assert not any('python' in Path(v).name or 'tk8' in Path(v).name for v in modules)
        else:
            assert not (directory/'coverage.json').exists()
        checks.append(dict(name=name,passed=True))
    (out/'validation.json').write_text(json.dumps(dict(passed=True,checks=checks),indent=2))
    print(json.dumps(dict(passed=True,checks=len(checks))))


if __name__=='__main__':
    main()
