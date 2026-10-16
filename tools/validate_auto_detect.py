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
import ctypes
from ctypes import wintypes


class NoAppInitProcess:
    """Owned native fixture with extension points disabled at process creation."""
    def __init__(self, arguments):
        k = ctypes.WinDLL('kernel32', use_last_error=True)
        class Startup(ctypes.Structure):
            _fields_ = [('cb', wintypes.DWORD), ('reserved', wintypes.LPWSTR), ('desktop', wintypes.LPWSTR),
                        ('title', wintypes.LPWSTR), ('x', wintypes.DWORD), ('y', wintypes.DWORD),
                        ('cx', wintypes.DWORD), ('cy', wintypes.DWORD), ('charsx', wintypes.DWORD),
                        ('charsy', wintypes.DWORD), ('fill', wintypes.DWORD), ('flags', wintypes.DWORD),
                        ('show', wintypes.WORD), ('reserved2size', wintypes.WORD), ('reserved2', ctypes.c_void_p),
                        ('stdin', wintypes.HANDLE), ('stdout', wintypes.HANDLE), ('stderr', wintypes.HANDLE)]
        class StartupEx(ctypes.Structure):
            _fields_ = [('info', Startup), ('attributes', ctypes.c_void_p)]
        class ProcessInfo(ctypes.Structure):
            _fields_ = [('process', wintypes.HANDLE), ('thread', wintypes.HANDLE), ('pid', wintypes.DWORD), ('tid', wintypes.DWORD)]
        k.InitializeProcThreadAttributeList.argtypes = [ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(ctypes.c_size_t)]
        k.UpdateProcThreadAttribute.argtypes = [ctypes.c_void_p, wintypes.DWORD, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p]
        k.DeleteProcThreadAttributeList.argtypes = [ctypes.c_void_p]
        k.CreateProcessW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p, ctypes.c_void_p, wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR, ctypes.c_void_p, ctypes.POINTER(ProcessInfo)]
        k.CloseHandle.argtypes = [wintypes.HANDLE]
        k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        k.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        k.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        size = ctypes.c_size_t()
        k.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(size))
        storage = ctypes.create_string_buffer(size.value)
        if not k.InitializeProcThreadAttributeList(storage, 1, 0, ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            policy = ctypes.c_ulonglong(1 << 32)  # EXTENSION_POINT_DISABLE_ALWAYS_ON
            if not k.UpdateProcThreadAttribute(storage, 0, 0x20007, ctypes.byref(policy), ctypes.sizeof(policy), None, None):
                raise ctypes.WinError(ctypes.get_last_error())
            startup = StartupEx(); startup.info.cb = ctypes.sizeof(startup)
            startup.attributes = ctypes.addressof(storage)
            info = ProcessInfo(); command = ctypes.create_unicode_buffer(subprocess.list2cmdline(arguments))
            if not k.CreateProcessW(arguments[0], command, None, None, False, 0x80000, None, None, ctypes.byref(startup), ctypes.byref(info)):
                raise ctypes.WinError(ctypes.get_last_error())
            self.kernel, self.handle, self.pid = k, info.process, info.pid
            k.CloseHandle(info.thread)
        finally:
            k.DeleteProcThreadAttributeList(storage)

    def poll(self):
        if self.kernel.WaitForSingleObject(self.handle, 0) == 0x102:
            return None
        code = wintypes.DWORD()
        if not self.kernel.GetExitCodeProcess(self.handle, ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        return code.value

    def wait(self, timeout):
        if self.kernel.WaitForSingleObject(self.handle, int(timeout * 1000)) == 0x102:
            raise subprocess.TimeoutExpired('native fixture', timeout)
        return self.poll()

    def terminate(self):
        self.kernel.TerminateProcess(self.handle, 1)

    def __del__(self):
        if getattr(self, 'handle', None):
            self.kernel.CloseHandle(self.handle)


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
    parser.add_argument('--active', action='store_true', help='Select fixture EXE for active attachment')
    parser.add_argument('--no-appinit', action='store_true', help='Disable AppInit only in the owned fixture')
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
    if args.no_appinit and not args.active:
        parser.error('--no-appinit requires --active')
    monitor_args = [str(binary / 'FloraCapture.exe'), 'monitor', '--out', str(root / 'sessions')]
    if args.active:
        monitor_args += ['--target-exe', str(target_exe)]
    monitor = subprocess.Popen(monitor_args,
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
        if args.no_appinit:
            target = NoAppInitProcess([str(target_exe), str(oracle), args.mode])
        else:
            target = subprocess.Popen([str(target_exe), str(oracle), args.mode], env=environment,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        session = event('session')['session']
        assert session['pid'] == target.pid
        if args.no_appinit:
            assert any(e.get('phase') == 'remote_load' and e.get('pid') == target.pid for e in events)
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
        if args.active:
            deadline = time.monotonic() + 10
            while not any(e.get('phase') == 'graphics_confirmed' and e.get('pid') == target.pid for e in events):
                assert time.monotonic() < deadline, 'Broker did not confirm real D3D11 frame updates'
                time.sleep(.1)
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
        # RegFlushKey in the independent recovery guard can take tens of seconds
        # on this host. Observe recovery completion instead of racing that guard.
        recovery_started = time.monotonic()
        deadline = recovery_started + 60
        while registry() != before and time.monotonic() < deadline:
            time.sleep(.2)
        assert registry() == before
        (root / 'recovery-timing.json').write_text(json.dumps({'seconds': time.monotonic()-recovery_started}))
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
