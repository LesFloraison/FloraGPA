"""Run native MSAD and execution-journal regressions serially with retained binaries."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binaries', type=Path, required=True)
    parser.add_argument('--qt-bin', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    (root / 'binaries').mkdir()
    env = {k: v for k, v in os.environ.items() if not k.startswith('FLORA_DXBC_')}
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env.get('PATH', '')
    env['QT_QPA_PLATFORM'] = 'offscreen'
    env['FLORA_DXBC_MSAD_EVIDENCE'] = str(root / 'evidence')
    suites = ('FloraDxbcExecutionTests', 'FloraDxbcExecutionTraceTests',
              'FloraDxbcExecutionTraceReaderTests', 'FloraNativeDebugSessionTests',
              'FloraNativeComputeDebugTests')
    for name in (*suites, 'FloraGPA', 'FloraGPA.Cli', 'FloraGPA.Worker'):
        shutil.copy2(args.binaries / (name + '.exe'), root / 'binaries' / (name + '.exe'))
    results = []
    for name in suites:
        command = [str((args.binaries / (name + '.exe')).resolve()),
                   '-o', str(root / (name + '.txt')) + ',txt']
        start = time.monotonic()
        with (root / (name + '-console.txt')).open('w', encoding='utf-8') as log:
            result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=900)
        results.append({'suite': name, 'command': command, 'exit_code': result.returncode,
                        'seconds': time.monotonic() - start})
        (root / 'exits.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
        print(name, result.returncode, flush=True)
        if result.returncode:
            return result.returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
