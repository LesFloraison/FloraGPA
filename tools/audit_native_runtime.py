"""Audit every process/image debug event, including transient DLL loads.

Development-only Windows x64 debugger. No injection or installed GPA is needed.
The debug port follows child processes; workload results must be checked separately.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import subprocess
import time


def audit(command, output, timeout):
    kernel = c.WinDLL('kernel32', use_last_error=True)
    class Event(c.Structure):
        _fields_ = [('code', w.DWORD), ('pid', w.DWORD), ('tid', w.DWORD),
                    ('payload', c.c_uint64 * 20)]
    assert c.sizeof(Event) == 176
    kernel.WaitForDebugEvent.argtypes = [c.POINTER(Event), w.DWORD]
    kernel.ContinueDebugEvent.argtypes = [w.DWORD, w.DWORD, w.DWORD]
    kernel.GetFinalPathNameByHandleW.argtypes = [w.HANDLE, w.LPWSTR, w.DWORD, w.DWORD]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.DebugSetProcessKillOnExit.argtypes = [w.BOOL]
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
    kernel.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
    def image_path(handle):
        if not handle:
            return None
        buf = c.create_unicode_buffer(32768)
        length = kernel.GetFinalPathNameByHandleW(handle, buf, len(buf), 0)
        if not 0 < length < len(buf):
            return None
        return buf.value.removeprefix('\\\\?\\')
    def process_path(pid):
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            return None
        try:
            buf = c.create_unicode_buffer(32768)
            length = w.DWORD(len(buf))
            return buf.value if kernel.QueryFullProcessImageNameW(handle, 0, buf, c.byref(length)) else None
        finally:
            kernel.CloseHandle(handle)
    output.mkdir(parents=True, exist_ok=False)
    report = {'command': command, 'method': 'Windows DEBUG_PROCESS process/image events',
              'processes': [], 'images': [], 'image_unloads': [], 'exceptions': [], 'unresolved_images': [],
              'timed_out': False, 'completed': False}
    active = set()
    images = {}
    initial_breaks = set()
    started = time.monotonic()
    env = {key: value for key, value in os.environ.items()
           if key.upper() in {'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA'}}
    env['PATH'] = os.environ['WINDIR'] + ';' + os.environ['WINDIR'] + '/System32'
    with (output / 'process.log').open('wb') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=env,
                                   creationflags=0x1 | subprocess.CREATE_NO_WINDOW)
        kernel.DebugSetProcessKillOnExit(True)
        active.add(process.pid)
        try:
            while active:
                if time.monotonic() - started > timeout:
                    report['timed_out'] = True
                    raise TimeoutError('Runtime audit timed out')
                event = Event()
                if not kernel.WaitForDebugEvent(c.byref(event), 100):
                    if c.get_last_error() == 121:
                        continue
                    raise c.WinError(c.get_last_error())
                status = 0x00010002  # DBG_CONTINUE
                payload = bytes(event.payload)
                try:
                    if event.code in (3, 6):  # CREATE_PROCESS / LOAD_DLL
                        handle = event.payload[0]
                        path = image_path(handle)
                        if handle:
                            kernel.CloseHandle(handle)
                        if event.code == 3:
                            active.add(event.pid)
                            path = path or process_path(event.pid)
                            report['processes'].append({'pid': event.pid, 'path': path})
                        base = event.payload[3 if event.code == 3 else 1]
                        images[event.pid, base] = path
                        row = {'pid': event.pid, 'path': path, 'event': event.code, 'base': hex(base)}
                        report['images'].append(row)
                        if path is None:
                            report['unresolved_images'].append(row)
                    elif event.code == 7:  # UNLOAD_DLL
                        base = event.payload[0]
                        report['image_unloads'].append({'pid': event.pid, 'base': hex(base),
                                                       'path': images.pop((event.pid, base), None)})
                    elif event.code == 5:  # EXIT_PROCESS
                        active.discard(event.pid)
                        code = int.from_bytes(payload[:4], 'little')
                        for row in report['processes']:
                            if row['pid'] == event.pid:
                                row['exit_code'] = code
                    elif event.code == 1:  # EXCEPTION_DEBUG_EVENT
                        code = int.from_bytes(payload[:4], 'little')
                        first_chance = int.from_bytes(payload[152:156], 'little')
                        if code == 0x80000003 and event.pid not in initial_breaks:
                            initial_breaks.add(event.pid)
                        else:
                            status = 0x80010001  # DBG_EXCEPTION_NOT_HANDLED
                            report['exceptions'].append({'pid': event.pid, 'code': hex(code),
                                                         'first_chance': bool(first_chance)})
                finally:
                    if not kernel.ContinueDebugEvent(event.pid, event.tid, status):
                        raise c.WinError(c.get_last_error())
            report['root_exit_code'] = process.wait(timeout=10)
            report['completed'] = True
        finally:
            for pid in active:
                handle = kernel.OpenProcess(1, False, pid)
                if handle:
                    kernel.TerminateProcess(handle, 1)
                    kernel.CloseHandle(handle)
            report['seconds'] = round(time.monotonic() - started, 3)
            forbidden = ('renderdoc', '/intelswtools/gpa/', 'dx11_player.dll', 'dx11_playback.dll',
                         'shimd3d64.dll', 'framedb_qt.dll', '/gpa.dll')
            report['forbidden_images'] = [row for row in report['images']
                if any(token in (row['path'] or '').replace('\\', '/').lower() for token in forbidden)]
            report['audit_passed'] = (report['completed'] and not report['unresolved_images']
                                      and not report['forbidden_images'])
            (output / 'audit.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command or args.timeout <= 0:
        parser.error('Provide a command and positive timeout')
    result = audit(command, args.out, args.timeout)
    print(json.dumps({key: result[key] for key in ('audit_passed', 'root_exit_code', 'seconds')}))
    raise SystemExit(0 if result['audit_passed'] and result['root_exit_code'] == 0 else 1)
