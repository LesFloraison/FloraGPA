"""Compare Python and colocated native research hosts with serial GPA probes.

Copies installed root DLLs into a new ignored sandbox and makes the copies
read-only. Never edits the installed GPA files. No product uses this launcher.
"""
import argparse
import json
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import time

from probe_original_debugger_host import digest


def identity(path):
    return dict(path=str(path.resolve()), bytes=path.stat().st_size, sha256=digest(path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--watcher', type=Path, required=True)
    parser.add_argument('--gpa', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--event', type=int, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if not 0 < args.event <= 0xffffffff:
        parser.error('Expected a positive uint32 event')
    tools = Path(__file__).resolve().parent
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    sandbox = args.out / 'modules'
    sandbox.mkdir()
    runtime = Path(sys.prefix)
    library = runtime / f'python{sys.version_info.major}{sys.version_info.minor}.dll'
    report = dict(schema='FloraGPA host environment runs 1', completed=False,
                  engine_recovered=False, shader_debug_parity=False,
                  copied_files=[], runs=[], sources=[identity(path) for path in (
                      Path(__file__), tools / 'probe_original_host_environment.py',
                      tools / 'probe_original_debugger_host.py', tools / 'probe_original_module_routes.py',
                      tools / 'native/original_research_host.cpp', args.host, args.watcher, library)],
                  capture=identity(args.capture))
    def save():
        (args.out / 'runs.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    def run(name, command, timeout=130, expected=0):
        row = dict(name=name, command=command, expected_exit=expected, timed_out=False)
        report['runs'].append(row)
        save()
        started = time.monotonic()
        with (args.out / (name + '.log')).open('w', encoding='utf-8') as log:
            try:
                result = subprocess.run(command, cwd=args.out, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=timeout, creationflags=subprocess.CREATE_NO_WINDOW)
                row['exit_code'] = result.returncode
            except subprocess.TimeoutExpired:
                row.update(exit_code=None, timed_out=True)
        row['elapsed_ms'] = round((time.monotonic() - started) * 1000)
        save()
        if row['exit_code'] != expected:
            raise RuntimeError('Unexpected child outcome: ' + name)
        print(name, row['elapsed_ms'], 'ms', flush=True)
    try:
        for source in sorted(args.gpa.glob('*.dll')):
            before = identity(source)
            target = sandbox / source.name
            shutil.copyfile(source, target)
            target.chmod(stat.S_IREAD)
            copied = identity(target)
            if before['sha256'] != copied['sha256']:
                raise ValueError('Copy changed: ' + source.name)
            report['copied_files'].append(dict(original=before, copy=copied))
        executable = sandbox / args.host.name
        shutil.copyfile(args.host, executable)
        executable.chmod(stat.S_IREAD)
        report['launcher_copy'] = identity(executable)
        prefix = [str(executable), str(runtime), str(library)]
        save()
        run('usage', [str(executable)], 15, 2)
        run('missing-runtime', [str(executable), str(runtime), str(args.out / 'missing.dll'),
                                str(tools / 'probe_original_host_environment.py')], 15, 2)
        run('wrong-runtime', [str(executable), str(runtime), 'C:/Windows/System32/kernel32.dll',
                              str(tools / 'probe_original_host_environment.py')], 15, 3)
        for label, command in [('python', [sys.executable, '-E', '-s']), ('colocated', prefix)]:
            run(label, command + [str(tools / 'probe_original_host_environment.py'),
                                  '--gpa', str(sandbox), '--out', str(args.out / (label + '.json'))], 30)
        for index in range(3):
            run(f'calibration-{index}', [str(args.watcher.resolve()),
                                        str(args.out / f'calibration-{index}.json'), '--self-test'])
        combinations = [(label, command, original, vendor)
                        for label, command in [('python', [sys.executable, '-E', '-s']), ('colocated', prefix)]
                        for original in (False, True) for vendor in (4318, 32902)]
        for label, launcher, original, vendor in combinations:
            name = label + '-' + ('original' if original else 'controlled') + '-' + str(vendor)
            directory = args.out / name
            directory.mkdir()
            command = [str(args.watcher.resolve()), str(directory / 'watch.json'), *launcher,
                       str(tools / 'probe_original_debugger_host.py'), str(args.capture.resolve()),
                       '--event', str(args.event), '--gpa', str(sandbox), '--out', str(directory),
                       '--vendor', str(vendor), '--host-graph', '--child']
            if original:
                command.append('--original-initialization')
            run(name, command)
        report['completed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        report['files_unchanged'] = all(digest(Path(row[side]['path'])) == row[side]['sha256']
                                        for row in report['copied_files'] for side in ('original', 'copy'))
        report['capture_unchanged'] = digest(args.capture) == report['capture']['sha256']
        save()
    if not report['files_unchanged'] or not report['capture_unchanged']:
        raise ValueError('Research inputs changed')


if __name__ == '__main__':
    main()
