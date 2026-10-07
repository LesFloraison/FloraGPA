"""CPU-only process-lifecycle controls for the persistent recovery runner."""
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from validate_recovery_soak import run_process


@unittest.skipUnless(os.name == 'nt', 'Windows process-tree semantics')
class ProcessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='Flora recovery process ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def run_child(self, program, timeout=10):
        with (self.root / 'process.log').open('wb') as log:
            return run_process([sys.executable, '-c', program], self.root,
                               os.environ.copy(), log, timeout)

    def test_success(self):
        self.assertEqual(self.run_child('print("complete")'), 0)
        self.assertIn(b'complete', (self.root / 'process.log').read_bytes())

    def test_nonzero_is_preserved(self):
        self.assertEqual(self.run_child('raise SystemExit(9)'), 9)

    def test_timeout_terminates_parent_and_child(self):
        program = '''import json, os, subprocess, sys, time
from pathlib import Path
child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(120)'],
                         creationflags=subprocess.CREATE_NO_WINDOW)
Path('pids.json').write_text(json.dumps([os.getpid(), child.pid]))
time.sleep(120)
'''
        with self.assertRaises(subprocess.TimeoutExpired):
            self.run_child(program, timeout=3)
        pids = json.loads((self.root / 'pids.json').read_text())
        self.assertEqual(len(pids), 2)
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        for pid in pids:
            handle = kernel.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
            if not handle:
                self.assertEqual(ctypes.get_last_error(), 87, f'Cannot inspect PID {pid}')
                continue  # ERROR_INVALID_PARAMETER: the process no longer exists.
            try:
                self.assertEqual(kernel.WaitForSingleObject(handle, 5000), 0,
                                 f'Timed-out process {pid} remains alive')
            finally:
                kernel.CloseHandle(handle)


if __name__ == '__main__':
    unittest.main()
