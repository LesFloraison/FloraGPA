"""Run depth return-count and isolated-history regressions serially."""
import argparse, json, os, shutil, subprocess, sys, time
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('binaries','qt-bin','captures','out'):p.add_argument('--'+key,type=Path,required=True)
    a=p.parse_args();root=a.out.resolve();root.mkdir(parents=True,exist_ok=False)
    env=dict(os.environ);env['PATH']=str(a.qt_bin.resolve())+os.pathsep+env.get('PATH','');env['QT_QPA_PLATFORM']='offscreen'
    env={k:v for k,v in env.items() if not k.startswith(('FLORA_PIXEL_','FLORA_DRAW_HISTORY_'))}
    rows=[];(root/'binaries').mkdir()
    for suite in ('FloraDxbcPixelCompletionTests','FloraNativePixelReturnCountTests'):
        binary=a.binaries.resolve()/(suite+'.exe');shutil.copy2(binary,root/'binaries'/binary.name)
        extra={'FLORA_PIXEL_COMPLETION_EVIDENCE':str(root/'depth')} if 'Completion' in suite else {}
        start=time.monotonic()
        with (root/(suite+'-console.txt')).open('w',encoding='utf-8') as log:
            status=subprocess.run([str(binary),'-o',str(root/(suite+'.txt'))+',txt'],env=env|extra,
                stdout=log,stderr=subprocess.STDOUT,timeout=900).returncode
        rows.append({'suite':suite,'exit_code':status,'seconds':time.monotonic()-start})
        (root/'exits.json').write_text(json.dumps(rows,indent=2)+'\n',encoding='utf-8');print(suite,status,flush=True)
        if status:return status
    return subprocess.run([sys.executable,str(Path(__file__).with_name('run_history_return_count.py')),
        '--binaries',str(a.binaries),'--qt-bin',str(a.qt_bin),'--captures',str(a.captures),'--out',str(root/'jobs')],env=env,timeout=1800).returncode
if __name__=='__main__':raise SystemExit(main())
