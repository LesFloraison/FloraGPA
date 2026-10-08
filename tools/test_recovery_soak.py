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

from validate_recovery_soak import digest, run_process, validate_workflows


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='Flora workflow audit ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.rows = []
        for index in range(1, 3):
            path = self.root / f'{index:06d}'
            (path / 'api').mkdir(parents=True)
            commands = dict(frame='original.gpa_frame', filter=dict(text='GetData', resource=None),
                            commands=[dict(id=42)])
            (path / 'api/commands.json').write_text(json.dumps(commands), encoding='utf-8')
            (path / 'api/commands.csv').write_text('id,api\n42,GetData\n', encoding='utf-8-sig')
            for name in ['contexts.json', 'command-lists.json']:
                (path / name).write_text('{}', encoding='utf-8')
            self.rows.append(dict(capture='original.gpa_frame', workflows=dict(
                directory=path.name, cancelled_choosers=2, query_events=[42], files={})))
            self.refresh(index - 1)

    def refresh(self, index):
        row = self.rows[index]['workflows']
        row['files'] = {p.relative_to(self.root / row['directory']).as_posix():
                        dict(bytes=p.stat().st_size, sha256=digest(p))
                        for p in (self.root / row['directory']).rglob('*') if p.is_file()}

    def test_complete(self):
        self.assertEqual(len(validate_workflows(self.root, self.rows)), 8)

    def test_modified_bytes(self):
        (self.root / '000001/contexts.json').write_text('{"modified":true}', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'identity mismatch'):
            validate_workflows(self.root, self.rows)

    def test_missing_file(self):
        (self.root / '000001/contexts.json').unlink()
        with self.assertRaises(FileNotFoundError):
            validate_workflows(self.root, self.rows)

    def test_wrong_ui_inventory(self):
        self.rows[0]['workflows']['query_events'] = [43]
        with self.assertRaisesRegex(ValueError, 'event inventory'):
            validate_workflows(self.root, self.rows)

    def test_wrong_csv(self):
        (self.root / '000001/api/commands.csv').write_text('id,api\n43,GetData\n', encoding='utf-8')
        self.refresh(0)
        with self.assertRaisesRegex(ValueError, 'CSV and JSON'):
            validate_workflows(self.root, self.rows)

    def test_valid_but_unstable_structure(self):
        (self.root / '000002/contexts.json').write_text('{"changed":true}', encoding='utf-8')
        self.refresh(1)
        with self.assertRaisesRegex(ValueError, 'Repeated workflow'):
            validate_workflows(self.root, self.rows)

    def test_structure_error(self):
        (self.root / '000001/command-lists.json').write_text('{"error":"failed"}', encoding='utf-8')
        self.refresh(0)
        with self.assertRaisesRegex(ValueError, 'did not complete'):
            validate_workflows(self.root, self.rows)

    def test_cycle_path_escape(self):
        self.rows[0]['workflows']['directory'] = '../outside'
        with self.assertRaisesRegex(ValueError, 'cycle'):
            validate_workflows(self.root, self.rows)


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
