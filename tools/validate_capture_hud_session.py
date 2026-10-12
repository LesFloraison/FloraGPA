"""Development-only serial injected HUD isolation experiment; Python is not shipped."""
import argparse
import json
import subprocess
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--mode', choices=['single', 'blt', 'flip', 'flip3', 'borderless', 'exclusive'], required=True)
    args = parser.parse_args()
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    binary = args.bin.resolve()
    session = root/'session/session.json'
    target = root/'target'
    target.mkdir()

    def cli(*arguments):
        result = subprocess.run([str(binary/'FloraCapture.exe'), *map(str, arguments)], capture_output=True, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stderr.decode(errors='replace')+result.stdout.decode(errors='replace'))
        return json.loads(result.stdout)

    def wait(test, why):
        deadline = time.monotonic()+60
        while time.monotonic() < deadline:
            error = target/'error.json'
            if error.exists():
                raise RuntimeError(error.read_text())
            result = test()
            if result:
                return result
            time.sleep(.1)
        raise TimeoutError(why)

    def status():
        return cli('status', '--session', session)

    launched = cli('launch', '--exe', binary/'FloraCaptureHudLiveTarget.exe', '--out', root/'session', '--', target, args.mode)
    (root/'launch.json').write_text(json.dumps(launched, indent=2))
    try:
        wait(lambda: (target/'ready.json').exists(), 'Target initialization')
        before = wait(lambda: (s if (s := status())['hud']['visible'] else None), 'Visible HUD')
        (root/'before.json').write_text(json.dumps(before, indent=2))
        modules = [Path(m).name.lower() for m in before['loaded_modules']]
        assert not any(any(token in name for token in ('qt6', 'python', 'renderdoc', 'floracore', 'florareplay', 'dx11_player', 'shimd3d')) for name in modules)
        if args.mode != 'exclusive':
            (target/'exercise.signal').touch()
            wait(lambda: (target/'exercise.json').exists(), 'Present and Resize exercise')
        requests = []
        for _ in range(3):
            requests.append(cli('capture', '--session', session, '--swap-chain', '1'))
            completed = wait(lambda: (s if not (s := status())['capture_progress']['busy'] else None), 'Capture completion')
            assert completed['capture_progress']['state'] == 'Completed', completed['capture_progress']
            requests.append(completed['capture_progress'])
        (root/'requests.json').write_text(json.dumps(requests, indent=2))
        after = status()
        (root/'after.json').write_text(json.dumps(after, indent=2))
        assert after['hud']['visible']
        assert len(list((root/'session').glob('*.gpa_frame'))) == 3
    finally:
        disconnected = cli('disconnect', '--session', session)
        (root/'disconnect.json').write_text(json.dumps(disconnected, indent=2))
        (target/'finish.signal').touch()
        wait(lambda: (target/'exit.json').exists(), 'Graceful target exit')
    result = json.loads((target/'exit.json').read_text())
    assert result['exact_retained_pixels'], result
    print(json.dumps(result))


if __name__ == '__main__':
    main()
