"""Run native pixel attribute evaluation and debugger regressions serially."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def digest(path):
    raw = path.read_bytes()
    return {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binaries', type=Path, required=True)
    parser.add_argument('--qt-bin', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--workflow-only', action='store_true')
    args = parser.parse_args()
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    (root / 'binaries').mkdir()
    suites = [('FloraDxbcPixelInputsTests', ['capture', 'attributeRequestValidation']),
              ('FloraDxbcExecutionTests', []), ('FloraDxbcExecutionTraceTests', []),
              ('FloraDxbcExecutionTraceReaderTests', []), ('FloraNativePixelOutputsTests', []),
              ('FloraNativePixelDebugTests', []), ('FloraNativeDebugSessionTests', []),
              ('FloraNativeComputeDebugTests', [])]
    binaries = {}
    for name in [item[0] for item in suites] + ['FloraGPA', 'FloraGPA.Worker', 'FloraGPA.Cli']:
        source = args.binaries / (name + '.exe')
        shutil.copy2(source, root / 'binaries' / source.name)
        binaries[source.name] = digest(source)
    (root / 'binary-identities.json').write_text(json.dumps(binaries, indent=2) + '\n', encoding='utf-8')
    env = {k: v for k, v in os.environ.items() if not k.startswith('FLORA_')}
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env.get('PATH', '')
    env['QT_QPA_PLATFORM'] = 'offscreen'
    env['FLORA_PIXEL_INPUT_EVIDENCE'] = str(root / 'inputs')
    env['FLORA_PIXEL_DEBUG_EVIDENCE'] = str(root / 'jobs')
    results = []
    for name, selection in suites:
        if args.workflow_only and name not in ('FloraNativePixelDebugTests', 'FloraNativeDebugSessionTests', 'FloraNativeComputeDebugTests'):
            continue
        command = [str((args.binaries / (name + '.exe')).resolve()), *selection,
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
