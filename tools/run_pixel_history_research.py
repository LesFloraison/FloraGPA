"""Run serial independent draw references and pinned original history requests."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from probe_original_debugger_host import digest


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--producer',type=Path,required=True)
    p.add_argument('--cli',type=Path,required=True)
    p.add_argument('--qt-bin',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--gpa',type=Path,default=Path('C:/Program Files/IntelSWTools/GPA'))
    args=p.parse_args()
    root=args.out.resolve(); root.mkdir(parents=True,exist_ok=False)
    repo=Path(__file__).resolve().parents[1]
    frozen=root/'producer'; frozen.mkdir()
    paths=['tools/native/pixel_history_probe.cpp','tools/native/original_capture_control.h',
           'tools/native/texture_probe_helpers.h','tools/native/present_capture_probe.cpp',
           'tools/probe_original_pixel_history.py','tools/probe_original_debugger_host.py',
           'tools/run_pixel_history_research.py']
    for path in [repo/x for x in paths]+[args.producer.resolve()]:
        shutil.copy2(path,frozen/path.name)
    producer=frozen/args.producer.name
    report=dict(schema=1,scope='original_pixel_history_research',completed=False,
                producer_files={f.name:digest(f) for f in frozen.iterdir()},
                cli=dict(path=str(args.cli.resolve()),sha256=digest(args.cli)),runs=[],
                gpa_sha256={n:digest(args.gpa/n) for n in ('dx11_player.dll','shimloader64.dll','shimd3d64.dll')})
    def save(): (root/'manifest.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    def run(label,command,capture=False):
        env=dict(os.environ); env.pop('GPA_LOCAL_INJECT',None)
        env['PATH']=str(args.qt_bin.resolve())+os.pathsep+env['PATH']
        if capture: env['GPA_LOCAL_INJECT']='true'
        row=dict(label=label,command=list(map(str,command)),completed=False); report['runs'].append(row); save()
        with (root/(label+'.log')).open('w',encoding='utf-8') as log:
            try:
                result=subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=150,
                                      creationflags=subprocess.CREATE_NO_WINDOW)
                row['exit_code']=result.returncode
            except subprocess.TimeoutExpired:
                row['timed_out']=True; save(); raise
        row['completed']=result.returncode==0; save()
        if result.returncode: raise ValueError('Research run failed: '+label)
    for vendor in (0,4318,32902):
        run('reference-'+str(vendor),[str(producer),str(root/('reference-'+str(vendor))),str(vendor)])
    capture=root/'fixture.gpa_frame'
    run('captured',[str(producer),str(root/'captured'),'4318',str(capture),str((args.gpa/'shimloader64.dll').resolve())],True)
    report['capture']=dict(path=str(capture),sha256=digest(capture)); save()
    run('commands',[str(args.cli.resolve()),'commands',str(capture),'--out',str(root/'commands')])
    commands=json.loads((root/'commands/commands.json').read_text())['commands']
    events=[c['id'] for c in commands if c['name']=='Draw']
    targets={r['value'] for c in commands if c['name']=='ClearRenderTargetView' for r in c['fields'] if r['name']=='view'}
    if len(events)!=12 or len(targets)!=1: raise ValueError('Unexpected captured draw/target sequence')
    target=next(iter(targets)); report.update(events=events,target=target); save()
    for vendor in (4318,32902):
        for xy in (4,1):
            label=f'history-{vendor}-{xy}'
            run(label,[sys.executable,str(frozen/'probe_original_pixel_history.py'),str(capture),
                       '--resource',str(target),'--x',str(xy),'--y',str(xy),'--event',*map(str,events),
                       '--vendor',str(vendor),'--observe','--gpa',str(args.gpa.resolve()),'--out',str(root/label)])
    report['gpa_unchanged']=all(digest(args.gpa/n)==value for n,value in report['gpa_sha256'].items())
    report['capture_unchanged']=digest(capture)==report['capture']['sha256']
    report['completed']=report['gpa_unchanged'] and report['capture_unchanged']
    save()
    if not report['completed']: raise ValueError('Original research files changed')
    print(str(root/'manifest.json'))


if __name__=='__main__': main()
