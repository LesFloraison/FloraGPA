"""Run one bounded recovery attempt and unstarted cases after a retained timeout."""
import argparse
import json
from pathlib import Path
import subprocess
import time

from run_original_host_environment import identity
from probe_original_debugger_host import digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    original = json.loads(args.manifest.read_text(encoding='utf-8'))
    if original['completed'] or not original['files_unchanged'] or not original['capture_unchanged']:
        raise ValueError('Expected an incomplete, input-preserving research run')
    for source in original['sources'] + [original['capture'], original['launcher_copy']]:
        if digest(Path(source['path'])) != source['sha256']:
            raise ValueError('Research source changed')
    for row in original['copied_files']:
        for side in ('original', 'copy'):
            if digest(Path(row[side]['path'])) != row[side]['sha256']:
                raise ValueError('Research DLL changed')
    failed = original['runs'][-1]
    # This recovery is deliberately restricted to the observed pre-request timeout.
    watch_path = Path(failed['command'][1])
    watch = json.loads(watch_path.read_text(encoding='utf-8'))
    if (failed['name'] != 'colocated-controlled-32902' or failed['exit_code'] != 1 or
            watch.get('error') != 'Hardware watch timed out' or watch['timeout_ms'] != 120000):
        raise ValueError('Unexpected recovery boundary')
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    report = dict(schema='FloraGPA host environment recovery 1', completed=False,
                  original_manifest=identity(args.manifest), sources=[identity(Path(__file__))], runs=[])
    def save():
        (args.out / 'runs.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    try:
        for name, vendor, original_initialization in [('colocated-controlled-32902', 32902, False),
                ('colocated-original-4318', 4318, True), ('colocated-original-32902', 32902, True)]:
            directory = args.out / name
            directory.mkdir()
            command = list(failed['command'])
            command[1] = str(directory / 'watch.json')
            command[command.index('--out') + 1] = str(directory)
            command[command.index('--vendor') + 1] = str(vendor)
            if original_initialization:
                command.append('--original-initialization')
            row = dict(name=name, command=command, expected_exit=0, timed_out=False)
            report['runs'].append(row)
            save()
            started = time.monotonic()
            with (args.out / (name + '.log')).open('w', encoding='utf-8') as log:
                try:
                    result = subprocess.run(command, cwd=args.manifest.resolve().parent, stdout=log,
                                            stderr=subprocess.STDOUT, timeout=130,
                                            creationflags=subprocess.CREATE_NO_WINDOW)
                    row['exit_code'] = result.returncode
                except subprocess.TimeoutExpired:
                    row.update(exit_code=None, timed_out=True)
            row['elapsed_ms'] = round((time.monotonic() - started) * 1000)
            save()
            if row['exit_code'] != 0:
                raise RuntimeError('Bounded recovery failed: ' + name)
            print(name, row['elapsed_ms'], 'ms', flush=True)
        report['completed'] = True
    finally:
        report['files_unchanged'] = all(digest(Path(row[side]['path'])) == row[side]['sha256']
                                        for row in original['copied_files'] for side in ('original', 'copy'))
        report['capture_unchanged'] = digest(Path(original['capture']['path'])) == original['capture']['sha256']
        save()


if __name__ == '__main__':
    main()
