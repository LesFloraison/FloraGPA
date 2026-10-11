"""Development-only factory/COM oracle. GPU runs are deliberately serial."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    controller = args.bin.resolve() / 'FloraCapture.exe'
    probe = controller.with_name('FloraCaptureFactoryProbe.exe')
    checks = []

    def check(name, ok, **detail):
        checks.append(dict(name=name, passed=bool(ok), **detail))
        (out / 'validation.json').write_text(json.dumps(dict(
            passed=all(c['passed'] for c in checks), checks=checks,
            scope='Factory creation, COM identity and forwarding; not frame capture'), indent=2))
        if not ok:
            raise AssertionError(name)

    def ready(folder):
        deadline = time.monotonic() + 20
        while not (folder / 'ready.json').exists():
            if (folder / 'error.json').exists():
                raise RuntimeError((folder / 'error.json').read_text())
            if time.monotonic() >= deadline:
                raise TimeoutError(str(folder))
            time.sleep(.02)
        return json.loads((folder / 'ready.json').read_text())

    for mode in range(3):
        baseline = out / f'{mode}-native'
        native = subprocess.Popen([str(probe), str(baseline), str(mode)],
                                  creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            expected = ready(baseline)
        finally:
            if baseline.exists():
                (baseline / 'finish.signal').touch()
            native.wait(timeout=35)
        check(f'{mode}: native oracle', native.returncode == 0)
        injected = out / f'{mode}-injected'
        session = out / f'{mode}-session'
        run = subprocess.run([str(controller), 'launch', '--exe', str(probe), '--out', str(session),
                              '--', str(injected), str(mode)], capture_output=True, timeout=25)
        (out / f'{mode}-launch.log').write_bytes(run.stdout + run.stderr)
        try:
            check(f'{mode}: launch', run.returncode == 0)
            actual = ready(injected)
            check(f'{mode}: identity, counts, pixels and HRESULT match', actual == expected,
                  expected=expected, actual=actual)
            status = subprocess.run([str(controller), 'status', '--session', str(session / 'session.json')],
                                    capture_output=True, timeout=10)
            check(f'{mode}: status', status.returncode == 0)
            report = json.loads(status.stdout)
            (out / f'{mode}-status.json').write_text(json.dumps(report, indent=2))
            check(f'{mode}: factory tracked without registration errors',
                  report['factories']['active'] >= 1 and report['factories']['tracking_failures'] == 0)
            swap = report['swap_chains']
            check(f'{mode}: factory-created swap tracked once', len(swap['active']) == 1 and swap['created'] == 1
                  and swap['tracking_failures'] == 0 and swap['active'][0]['test_presents'] == 1)
            forbidden = [m for m in report['loaded_modules'] if any(token in m.replace('\\', '/').lower()
                         for token in ('intelswtools/gpa/', 'python3', 'qt6', 'renderdoc', 'floragpa.worker'))]
            check(f'{mode}: independent runtime modules', not forbidden, forbidden=forbidden)
            refused = subprocess.run([str(controller), 'capture', '--session', str(session / 'session.json')],
                                     capture_output=True, timeout=10)
            check(f'{mode}: incomplete capture explicitly rejected', refused.returncode == 3 and
                  json.loads(refused.stdout)['error'] == 'capture_capability_incomplete' and not list(session.glob('*.gpa_frame')))
        finally:
            if injected.exists():
                (injected / 'finish.signal').touch()
    (out / 'binaries.json').write_text(json.dumps({p.name: hashlib.sha256(p.read_bytes()).hexdigest()
         for p in (controller, probe, controller.with_name('FloraCaptureRuntime.dll'))}, indent=2))
    print(json.dumps(dict(passed=True, checks=len(checks))))


if __name__ == '__main__':
    main()
