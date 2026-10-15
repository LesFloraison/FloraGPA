"""Development-only serial Auto-Detect acceptance. Never shipped as a backend."""
import argparse
import json
import os
import queue
import shutil
import subprocess
import threading
import time
import winreg
from pathlib import Path


def registry():
    result = {}
    for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\Microsoft\Windows NT\CurrentVersion\Windows', 0, winreg.KEY_READ | view) as key:
            for name in ('AppInit_DLLs', 'LoadAppInit_DLLs', 'RequireSignedAppInit_DLLs'):
                try:
                    result[f'{view}:{name}'] = winreg.QueryValueEx(key, name)
                except FileNotFoundError:
                    result[f'{view}:{name}'] = None
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--target', default='FloraCaptureLiveTarget.exe')
    parser.add_argument('--mode', default='0')
    parser.add_argument('--stale-environment', action='store_true')
    parser.add_argument('--concurrent', action='store_true')
    parser.add_argument('--explicit-peer', action='store_true')
    parser.add_argument('--stop-kind', choices=['normal', 'owner-crash', 'broker-crash', 'residual'], default='normal')
    args = parser.parse_args()
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    binary = args.bin.resolve()
    target_exe = root / 'NativeAutoDetectProbe.exe'
    shutil.copy2(binary / args.target, target_exe)
    before = registry()
    (root / 'registry-before.json').write_text(json.dumps(before, indent=2))
    events = []
    messages = queue.Queue()
    target = None
    peer = None
    monitor = subprocess.Popen([str(binary / 'FloraCapture.exe'), 'monitor', '--out', str(root / 'sessions')],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')

    def consume():
        for line in monitor.stdout:
            events.append(json.loads(line))
            messages.put(events[-1])
    threading.Thread(target=consume, daemon=True).start()

    def event(kind):
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            message = messages.get(timeout=max(.1, deadline-time.monotonic()))
            if not message['ok']:
                raise RuntimeError(message)
            if message['event'] == kind:
                return message
        raise TimeoutError(kind)

    def cli(*arguments):
        result = subprocess.run([str(binary / 'FloraCapture.exe'), *map(str, arguments)], capture_output=True, timeout=25)
        value = json.loads(result.stdout)
        if result.returncode or not value.get('ok'):
            raise RuntimeError(value)
        return value

    try:
        ready = event('ready')
        active_registry = registry()
        # Starting a second UI must not restore a live manager's journal.
        cli('monitor-recover')
        assert registry() == active_registry
        oracle = root / 'target'
        environment = os.environ.copy()
        if args.stale_environment:
            environment.update(FLORAGPA_CAPTURE_SESSION='35402d8d-e3f5-458d-8dfb-b7466ad486a9',
                               FLORAGPA_CAPTURE_OUTPUT=str(root / 'stale-output'))
        target = subprocess.Popen([str(target_exe), str(oracle), args.mode], env=environment,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        session = event('session')['session']
        assert session['pid'] == target.pid
        manifest = session['session_file']
        forged = dict(session, process_created=session['process_created'] + 1)
        stale_manifest = root / 'stale-identity.json'
        stale_manifest.write_text(json.dumps(forged))
        rejected = subprocess.run([str(binary / 'FloraCapture.exe'), 'status', '--session', str(stale_manifest)], capture_output=True)
        assert rejected.returncode and not json.loads(rejected.stdout)['ok'], 'PID identity mismatch was accepted'
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            state = cli('status', '--session', manifest)
            if state['capture_available']:
                break
            time.sleep(.2)
        assert state['hook_status'] == 0 and state['device_creations'] > 0, state
        assert state['attachment'] == 'auto_detect'
        (root / 'status.json').write_text(json.dumps(state, indent=2))
        # The self-owned fixture has exactly one swap; desktop focus can move
        # while inspecting the test. Select its recorded identity explicitly.
        swaps = state['swap_chains']['active']
        assert len(swaps) == 1
        cli('capture', '--session', manifest, '--swap-chain', swaps[0]['id'])
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            state = cli('status', '--session', manifest)
            if not state['capture_progress']['busy']:
                break
            time.sleep(.2)
        (root / 'capture.json').write_text(json.dumps(state, indent=2))
        assert state['capture_progress']['state'] == 'Completed', state['capture_progress']
        if args.explicit_peer:
            explicit_oracle = root / 'explicit-target'
            try:
                launched = cli('launch', '--exe', target_exe, '--out', root / 'explicit-session',
                               '--', explicit_oracle, '3')
                explicit_manifest = launched['session_file']
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    explicit_state = cli('status', '--session', explicit_manifest)
                    if explicit_state['capture_available']:
                        break
                    time.sleep(.1)
                assert explicit_state['attachment'] == 'launch' and explicit_state['device_creations'] == 1
                assert explicit_state['objects']['interface_gaps'] == 0
                cli('capture', '--session', explicit_manifest, '--swap-chain', explicit_state['swap_chains']['active'][0]['id'])
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    explicit_state = cli('status', '--session', explicit_manifest)
                    if not explicit_state['capture_progress']['busy']:
                        break
                    time.sleep(.1)
                assert explicit_state['capture_progress']['state'] == 'Completed'
                assert len(list((root / 'sessions').glob('*/session.json'))) == 1, 'Explicit launch double-attached'
                (root / 'explicit-capture.json').write_text(json.dumps(explicit_state, indent=2))
                cli('disconnect', '--session', explicit_manifest)
            finally:
                explicit_oracle.mkdir(exist_ok=True)
                (explicit_oracle / 'finish.signal').touch()
        if args.concurrent:
            peer = subprocess.Popen([str(target_exe), str(root / 'peer'), '3'], env=environment)
            peer_session = event('session')['session']
            assert peer_session['pid'] == peer.pid and peer_session['session_id'] != session['session_id']
            peer_manifest = peer_session['session_file']
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                peer_state = cli('status', '--session', peer_manifest)
                if peer_state['capture_available']:
                    break
                time.sleep(.1)
            cli('capture', '--session', peer_manifest, '--swap-chain', peer_state['swap_chains']['active'][0]['id'])
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                peer_state = cli('status', '--session', peer_manifest)
                if not peer_state['capture_progress']['busy']:
                    break
                time.sleep(.1)
            assert peer_state['capture_progress']['state'] == 'Completed'
            assert cli('status', '--session', manifest)['capture_progress']['file'] == state['capture_progress']['file']
            (root / 'peer-capture.json').write_text(json.dumps(peer_state, indent=2))
            cli('disconnect', '--session', peer_manifest)
        if args.stop_kind == 'normal':
            monitor.stdin.write('stop\n')
            monitor.stdin.flush()
            event('stopped')
            assert monitor.wait(timeout=15) == 0
        elif args.stop_kind == 'owner-crash':
            monitor.kill()
            monitor.wait(timeout=15)
        else:
            if args.stop_kind == 'residual':
                subprocess.run(['taskkill', '/PID', str(ready['guard_pid']), '/F'], check=True, capture_output=True)
            # Kill only the exact broker just created by this test.
            subprocess.run(['taskkill', '/PID', str(ready['broker_pid']), '/F'], check=True, capture_output=True)
            monitor.wait(timeout=15)
            if args.stop_kind == 'residual':
                assert registry() == active_registry, 'Residual setup unexpectedly cleaned itself'
                inactive = root / 'inactive-lease'
                probe = subprocess.Popen([str(target_exe), str(inactive), '3'])
                try:
                    deadline = time.monotonic() + 15
                    while not (inactive / 'ready.json').exists() and time.monotonic() < deadline:
                        time.sleep(.1)
                    assert (inactive / 'ready.json').exists(), 'Inactive lease prevented ordinary rendering'
                    modules = subprocess.run(['powershell', '-NoProfile', '-Command',
                        f'(Get-Process -Id {probe.pid}).Modules | Select-Object -ExpandProperty ModuleName'],
                        capture_output=True, text=True, check=True).stdout
                    (root / 'inactive-lease-modules.txt').write_text(modules)
                    assert Path(ready['shim']).name.casefold() in modules.casefold()
                    assert 'floracaptureruntime.dll' not in modules.casefold()
                    assert len(list((root / 'sessions').glob('*/session.json'))) == 1
                finally:
                    inactive.mkdir(exist_ok=True)
                    (inactive / 'finish.signal').touch()
                    probe.wait(timeout=15)
                cli('monitor-recover')
        deadline = time.monotonic() + 15
        while registry() != before and time.monotonic() < deadline:
            time.sleep(.2)
        assert registry() == before
        assert cli('status', '--session', manifest)['capture_available']
        cli('disconnect', '--session', manifest)
    finally:
        if monitor.poll() is None:
            try:
                monitor.stdin.write('stop\n')
                monitor.stdin.flush()
                monitor.wait(timeout=15)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                monitor.kill()
                monitor.wait(timeout=5)
        if args.stop_kind == 'residual':
            cli('monitor-recover')
        if target:
            (root / 'target' / 'finish.signal').touch()
            try:
                target.wait(timeout=15)
            except subprocess.TimeoutExpired:
                target.terminate()
                target.wait(timeout=5)
        if peer:
            (root / 'peer' / 'finish.signal').touch()
            try:
                peer.wait(timeout=15)
            except subprocess.TimeoutExpired:
                peer.terminate()
                peer.wait(timeout=5)
        (root / 'events.json').write_text(json.dumps(events, indent=2))
        after = registry()
        (root / 'registry-after.json').write_text(json.dumps(after, indent=2))
        assert before == after, 'Global configuration was not restored'
    print(json.dumps({'ok': True, 'session': session, 'capture': state['capture_progress']}))


if __name__ == '__main__':
    main()
