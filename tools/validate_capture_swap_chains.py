"""Real DXGI COM/lifecycle oracle: compare injected and uninstrumented apps."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--bin',type=Path,required=True);a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    controller=a.bin.resolve()/'FloraCapture.exe';probe=a.bin.resolve()/'FloraCaptureSwapChainProbe.exe';checks=[]
    def check(name,ok,**details):
        checks.append(dict(name=name,passed=bool(ok),**details))
        (out/'validation.json').write_text(json.dumps(dict(passed=all(c['passed'] for c in checks),checks=checks,scope='Swap-chain wrapper and Present observations; not live frame capture'),indent=2));assert ok,name
    def wait_file(folder,name):
        deadline=time.monotonic()+15
        while not (folder/name).exists():
            if (folder/'error.json').exists():raise RuntimeError((folder/'error.json').read_text())
            if time.monotonic()>deadline:raise TimeoutError(name)
            time.sleep(.02)
        return json.loads((folder/name).read_text())
    def command(verb):
        result=subprocess.run([str(controller),verb,'--session',str(out/'session/session.json')],capture_output=True,timeout=20)
        check(verb+' control response',result.returncode==0)
        return json.loads(result.stdout)
    baseline=out/'baseline'
    with (out/'baseline.log').open('wb') as log:
        process=subprocess.Popen([str(probe),str(baseline)],stdout=log,stderr=log,creationflags=subprocess.CREATE_NO_WINDOW)
        wait_file(baseline,'held-failure.json');(baseline/'resize.signal').touch()
        before=wait_file(baseline,'ready.json');(baseline/'release.signal').touch()
        wait_file(baseline,'released.json');(baseline/'recreate.signal').touch()
        before_second=wait_file(baseline,'second.json');(baseline/'finish.signal').touch()
        check('Uninstrumented oracle succeeds',process.wait(timeout=20)==0)
    injected=out/'injected'
    launch=subprocess.run([str(controller),'launch','--exe',str(probe),'--out',str(out/'session'),'--',str(injected)],capture_output=True,timeout=20)
    (out/'launch.log').write_bytes(launch.stdout+b'\n'+launch.stderr);check('Injected oracle starts',launch.returncode==0)
    wait_file(injected,'held-failure.json')
    failed=command('status')['swap_chains']['active'][0]
    check('Failed Resize retains dimensions at the failure boundary',failed['resize_calls']==1 and failed['failed_resizes']==1 and (failed['width'],failed['height'])==(64,64))
    (injected/'resize.signal').touch();after=wait_file(injected,'ready.json')
    check('All versioned COM identities HRESULTs counts getters and pixels unchanged',before==after,before=before,after=after)
    check('Independent CPU pixels correct',after['first_image']['pixel_failures']==after['resized_image']['pixel_failures']==0)
    observed=command('status')['swap_chains'];(out/'tracking.json').write_text(json.dumps(observed,indent=2))
    check('One swap-chain object tracked',observed['created']==1 and observed['destroyed']==0 and len(observed['active'])==1 and observed['tracking_failures']==0)
    first=observed['active'][0]
    present1=int('present1_test' in after)
    successful=sum(p['result']==0 and not p['flags']&1 for p in after['presents'])
    unsuccessful=sum(p['result']!=0 and not p['flags']&1 for p in after['presents'])
    check('Present and Present1 each observed once',first['present_calls']==len(after['presents'])+present1 and first['present1_calls']==present1)
    check('TEST Present excluded from completed frame boundaries',first['test_presents']==1+present1 and first['successful_presents']==successful and first['unsuccessful_presents']==unsuccessful)
    check('Failed Resize does not replace descriptor; valid Resize updates it',first['resize_calls']==2 and first['failed_resizes']==1 and (first['width'],first['height'])==(80,48))
    check('Unwrapped resource/device outputs explicitly reported',first['interface_escapes']>=3 and 'GetBuffer' in first['last_interface_escape'])
    (injected/'release.signal').touch();wait_file(injected,'released.json')
    released=command('status')['swap_chains'];check('Last application Release unregisters wrapper',released['destroyed']==1 and released['active']==[])
    (injected/'recreate.signal').touch();second=wait_file(injected,'second.json')
    check('Recreated swap-chain pixels unchanged',second==before_second and second['pixel_failures']==0)
    latest=command('status')['swap_chains']
    check('New object receives a monotonic identity',latest['created']==2 and latest['destroyed']==1 and len(latest['active'])==1 and latest['active'][0]['id']>first['id'])
    check('New object has independent dimensions',latest['active'][0]['width']==32 and latest['active'][0]['height']==32)
    (injected/'finish.signal').touch();check('Injected oracle completes',wait_file(injected,'done.json')['ok'])
    (out/'binaries.json').write_text(json.dumps({f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in (probe,controller,controller.with_name('FloraCaptureRuntime.dll'))},indent=2))
    print(json.dumps(dict(passed=True,checks=len(checks))))


if __name__=='__main__':main()
