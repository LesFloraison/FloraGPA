"""Build and package a committed source archive without workspace build caches.

Development-only Python orchestration; neither the build nor the application
invokes Python. This checks the current host, not an independent clean machine.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import zipfile


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run_process(command, cwd, env, log, timeout):
    # Keep the parent alive until taskkill can find the complete MSBuild/compiler
    # tree. subprocess.run(timeout=...) kills only the direct child.
    with subprocess.Popen(command, cwd=cwd, env=env, stdout=log,
                          stderr=subprocess.STDOUT,
                          creationflags=subprocess.CREATE_NO_WINDOW) as process:
        try:
            return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            if process.poll() is None:
                try:
                    subprocess.run([str(Path(env['WINDIR'])/'System32/taskkill.exe'),
                                    '/PID', str(process.pid), '/T', '/F'],
                                   stdout=log, stderr=subprocess.STDOUT, timeout=15,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.wait()
            raise


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--qt-root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cmake', type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--test-target', action='append', default=[],
                        help='Also build this Qt test target from the same archive (repeatable; does not run tests)')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 64:
        parser.error('--jobs must be between 1 and 64')
    if any(not re.fullmatch(r'Flora[A-Za-z0-9]+Tests', name) for name in args.test_target):
        parser.error('--test-target must be a Flora...Tests CMake target')
    test_targets = list(dict.fromkeys(args.test_target))
    repo = Path(__file__).resolve().parents[1]
    qt = args.qt_root.resolve(strict=True)
    if not (qt/'bin/windeployqt.exe').is_file():
        parser.error('--qt-root must contain bin/windeployqt.exe')
    git = shutil.which('git')
    cmake = args.cmake or shutil.which('cmake')
    if not git or not cmake:
        parser.error('Git and CMake are required for this development check')
    cmake = Path(cmake).resolve(strict=True)
    commit = subprocess.check_output([git, 'rev-parse', 'HEAD'], cwd=repo, text=True).strip()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = output/'source workspace'
    archive = output/'source.zip'
    report = dict(schema='FloraGPA committed source build 1', completed=False,
                  source_commit=commit, verifier_sha256=digest(Path(__file__)),
                  test_targets=test_targets,
                  steps=[], limits=[
                      'Committed source only; uncommitted changes are not included',
                      'Fresh source and process environment on this host, not a clean-machine certification',
                      'Installed compiler, SDK, Qt and graphics driver remain available',
                      'Successful packaging does not prove GPU replay; run separate runtime checks'])

    def save():
        (output/'validation.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')

    try:
        subprocess.run([git, 'archive', '--format=zip', '--output='+str(archive), commit],
                       cwd=repo, check=True)
        report['archive_sha256'] = digest(archive)
        with zipfile.ZipFile(archive) as packed:
            names = packed.namelist()
            for name in names:
                path = Path(name)
                if path.is_absolute() or '..' in path.parts:
                    raise ValueError('Source archive path escapes its root')
                if path.parts[0] in {'build', 'out', 'artifacts', '.git', '.vs'} or path.name.lower() in {
                        'agent.md', 'agents.md', 'cmakeuserpresets.json'} or path.suffix.lower() in {
                        '.gpa_frame', '.gpaframe', '.sln', '.vcxproj'}:
                    raise ValueError('Local/generated content is tracked: '+name)
            packed.extractall(source)
        report['source_files'] = sum(not n.endswith('/') for n in names)
        allowed = {'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'USERPROFILE',
                   'LOCALAPPDATA', 'APPDATA', 'PROGRAMFILES', 'PROGRAMFILES(X86)',
                   'PROGRAMW6432', 'PROGRAMDATA', 'ALLUSERSPROFILE', 'HOMEDRIVE',
                   'HOMEPATH', 'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE',
                   'OS', 'SYSTEMDRIVE', 'PUBLIC', 'USERNAME', 'USERDOMAIN', 'PATHEXT'}
        env = {k: v for k, v in os.environ.items() if k.upper() in allowed}
        windows = Path(env['WINDIR'])
        env['PATH'] = os.pathsep.join(str(p) for p in [windows/'System32', windows,
            windows/'System32/Wbem', windows/'System32/WindowsPowerShell/v1.0'])
        # PowerShell 5.1 otherwise treats native EXEs differently and does not
        # reliably wait for their completion or set LASTEXITCODE.
        env.setdefault('PATHEXT', '.COM;.EXE;.BAT;.CMD')
        env['CL'] = '/MP'+str(args.jobs)
        report.update(environment_keys=sorted(env), path=env['PATH'], pathext=env['PATHEXT'])

        def run(name, command):
            step = dict(name=name, command=[str(a) for a in command])
            report['steps'].append(step)
            save()
            started = time.monotonic()
            with (output/(name+'.log')).open('wb') as log:
                code = run_process(step['command'], source, env, log, timeout=1800)
            step.update(exit_code=code, seconds=round(time.monotonic()-started, 3))
            save()
            if code:
                raise RuntimeError(name+' failed; see '+str(output/(name+'.log')))
            print(name, 'passed', flush=True)

        run('configure', [cmake, '--preset', 'vs2022', '-DCMAKE_PREFIX_PATH='+str(qt),
                          '-DBUILD_TESTING=OFF'])
        if not (source/'build/vs2022/FloraGPA.sln').is_file():
            raise RuntimeError('Configured solution is missing')
        run('build', [cmake, '--build', '--preset', 'release', '--parallel', args.jobs])
        package = output/'portable'
        run('package', [windows/'System32/WindowsPowerShell/v1.0/powershell.exe',
                        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                        source/'tools/package.ps1', '-OutputDirectory', package])
        report['package_files'] = {p.relative_to(package).as_posix(): digest(p)
                                   for p in package.rglob('*') if p.is_file()}
        if test_targets:
            # Package production first, then build selected tests against exactly
            # these sources. Never attach test executables from the caller's
            # incremental workspace build to the committed-source evidence.
            release = source/'build/vs2022/Release'
            production = {name: digest(release/name) for name in report['package_files']
                          if name.startswith('FloraGPA.') and (release/name).is_file()}
            run('configure-tests', [cmake, '--preset', 'vs2022',
                                    '-DCMAKE_PREFIX_PATH='+str(qt), '-DBUILD_TESTING=ON'])
            run('build-tests', [cmake, '--build', '--preset', 'release', '--parallel',
                                args.jobs, '--target', *test_targets])
            report['test_files'] = {p.name: digest(p) for p in sorted(release.glob('Flora*.exe'))
                                    if not p.name.startswith('FloraGPA.')}
            for target in test_targets:
                if target+'.exe' not in report['test_files']:
                    raise RuntimeError('Requested test executable is missing: '+target)
            if any(digest(release/name) != expected for name, expected in production.items()):
                raise RuntimeError('Building tests changed a packaged production binary')
            if any(digest(package/name) != expected for name, expected in report['package_files'].items()):
                raise RuntimeError('Building tests changed the portable package')
            report['production_unchanged_after_test_build'] = True
        report['completed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
