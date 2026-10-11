"""Build/test/package committed native capture sources without Qt or reference files."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    cmake=shutil.which('cmake');ctest=shutil.which('ctest')
    if not cmake or not ctest:raise RuntimeError('CMake and CTest are required')
    commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
    archive=out/'source.zip'
    subprocess.run(['git','archive','--format=zip','--output',str(archive),commit],cwd=root,check=True)
    source=out/'source with spaces'
    with zipfile.ZipFile(archive) as files:files.extractall(source)
    manifest=json.loads((source/'src/capture/dependencies.json').read_text())
    for name,expected in manifest['sha256'].items():
        if hashlib.sha256((source/name).read_bytes()).hexdigest()!=expected:
            raise RuntimeError('Archived dependency hash mismatch: '+name)
    env={k:v for k,v in os.environ.items() if k.upper() in {
        'SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA','USERPROFILE',
        'PROGRAMFILES','PROGRAMFILES(X86)','PROGRAMW6432','PATHEXT','PROGRAMDATA','ALLUSERSPROFILE',
        'HOMEDRIVE','HOMEPATH','NUMBER_OF_PROCESSORS','PROCESSOR_ARCHITECTURE','OS','SYSTEMDRIVE','PUBLIC','USERNAME','USERDOMAIN'}}
    env['PATH']=str(Path(os.environ['WINDIR'])/'System32')
    env['CL']='/MP4'
    build=out/'build';package=out/'package'
    evidence=dict(commit=commit,passed=False,scope='Native bootstrap and limited self-owned startup-injected capture; not GF2 acceptance',steps=[],
                  validator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),environment_keys=sorted(env))

    def save():
        (out/'validation.json').write_text(json.dumps(evidence,indent=2)+'\n')

    def run(name,command,timeout=240):
        with (out/(name+'.log')).open('wb') as log:
            child=subprocess.Popen([str(x) for x in command],env=env,cwd=source,stdout=log,stderr=subprocess.STDOUT)
            try:code=child.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                subprocess.run([str(Path(os.environ['WINDIR'])/'System32/taskkill.exe'),'/PID',str(child.pid),'/T','/F'],
                               stdout=log,stderr=subprocess.STDOUT)
                child.wait();code=-1
        evidence['steps'].append(dict(name=name,exit_code=code));save()
        if code:raise RuntimeError(name+' failed; inspect '+str(out/(name+'.log')))

    save()
    run('configure',[cmake,'-S',source,'-B',build,'-G','Visual Studio 17 2022','-A','x64',
        '-DFLORA_BUILD_REPLAY=OFF','-DFLORA_BUILD_CAPTURE=ON','-DFLORA_BUILD_CAPTURE_UI=OFF','-DBUILD_TESTING=ON'])
    cache=(build/'CMakeCache.txt').read_text()
    if 'Qt6_DIR:' in cache or (build/'FloraReplay.vcxproj').exists():raise RuntimeError('Capture-only build discovered replay or Qt')
    run('build',[cmake,'--build',build,'--config','Release','--parallel','4'])
    run('ctest',[ctest,'--test-dir',build,'-C','Release','--parallel','1','--output-on-failure'])
    powershell=Path(os.environ['WINDIR'])/'System32/WindowsPowerShell/v1.0/powershell.exe'
    run('package',[powershell,'-NoProfile','-ExecutionPolicy','Bypass','-File',source/'tools/package_capture.ps1',
        '-BuildDirectory',build,'-OutputDirectory',package])
    run('package-control',[cmake,'-DCONTROLLER='+str(package/'FloraCapture.exe'),
        '-DPROBE='+str(build/'capture/Release/FloraCaptureFactoryProbe.exe'),
        '-DOUTPUT='+str(out/'packaged-control'),'-P',source/'tests/CaptureBootstrap.cmake'])
    status=json.loads(next((out/'packaged-control').glob('*/status.json')).read_text())
    forbidden=[m for m in status['loaded_modules'] if any(s in m.replace('\\','/').lower()
        for s in ('intelswtools/gpa/','python3','qt6','renderdoc'))]
    if forbidden:raise RuntimeError('Unexpected packaged runtime dependencies: '+repr(forbidden))
    run('package-live-control',[build/'capture/Release/FloraCaptureControllerTests.exe',package/'FloraCapture.exe',
        build/'capture/Release/FloraCaptureLiveTarget.exe',out/'packaged-live'])
    live=json.loads((out/'packaged-live/results.json').read_text())
    for session in live:
        forbidden=[m for m in session['module_status']['loaded_modules'] if any(s in m.replace('\\','/').lower()
            for s in ('intelswtools/gpa/','python3','qt6','renderdoc'))]
        if forbidden:raise RuntimeError('Unexpected packaged live dependencies: '+repr(forbidden))
    evidence['packaged_live_frames']=sum(len(session['frames']) for session in live)
    evidence['package_files']={p.relative_to(package).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
        for p in package.rglob('*') if p.is_file()}
    evidence.update(passed=True,no_qt_discovery=True,system_path_only=True,loaded_modules=status['loaded_modules'])
    save();print(json.dumps(dict(passed=True,commit=commit,steps=len(evidence['steps']))))


if __name__=='__main__':main()
