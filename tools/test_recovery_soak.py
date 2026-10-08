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

from validate_recovery_soak import digest, run_process, validate_workflows, validate_platform, validate_gui_resources, GUI_WORKFLOW_STAGES


class GuiResourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='Flora GUI resource audit ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        counts = dict(handles=10, gdi_objects=2, user_objects=1)
        snapshot = dict(schema='FloraGPA GUI resource snapshot 1', process_id=42,
                        qt_platform='windows', before=counts, after=counts,
                        native_windows_complete=True, modules_complete=True,
                        native_top_windows=[], qt_top_widgets=[], qt_windows=[],
                        modules=[dict(name='test.exe', path='C:/self/test.exe')])
        summary = {k:snapshot[k] for k in ['before','after','native_windows_complete','modules_complete']}
        self.journal = dict(gui_before_window=summary, gui_after_window_destroy=summary,
                            observations=[dict(gui_snapshot=summary)], retention_control={})
        names = ['before_window.json','after_window_destroy.json','cycle-000001.json']
        for key in ['before','after_test_history_clear','after_log_clear','after_pixmap_cache_clear']:
            self.journal['retention_control'][key] = dict(gui_snapshot=summary)
            names.append(f'control-{key}.json')
        for name in names:
            (self.root/name).write_text(json.dumps(snapshot), encoding='utf-8')

    def test_complete(self):
        self.assertEqual(len(validate_gui_resources(self.root,self.journal)),7)

    def test_missing_or_extra_snapshot(self):
        (self.root/'unknown.json').write_text('{}',encoding='utf-8')
        with self.assertRaisesRegex(ValueError,'inventory differs'):
            validate_gui_resources(self.root,self.journal)
        (self.root/'unknown.json').unlink()
        (self.root/'before_window.json').unlink()
        with self.assertRaisesRegex(ValueError,'inventory differs'):
            validate_gui_resources(self.root,self.journal)

    def test_incomplete_wrong_platform_or_other_process(self):
        path=self.root/'after_window_destroy.json';original=json.loads(path.read_text())
        for key,value,reason in [('modules_complete',False,'Incomplete'),
                                 ('native_windows_complete',False,'Incomplete'),
                                 ('qt_platform','offscreen','wrong-platform'),
                                 ('process_id',99,'different processes')]:
            with self.subTest(key=key):
                path.write_text(json.dumps(original|{key:value}),encoding='utf-8')
                with self.assertRaisesRegex(ValueError,reason):
                    validate_gui_resources(self.root,self.journal)

    def test_invalid_count(self):
        path=self.root/'cycle-000001.json';snapshot=json.loads(path.read_text())
        snapshot['after']['gdi_objects']=-1
        path.write_text(json.dumps(snapshot),encoding='utf-8')
        with self.assertRaisesRegex(ValueError,'Invalid GUI resource count'):
            validate_gui_resources(self.root,self.journal)

    def test_modified_summary(self):
        self.journal['gui_before_window']=dict(before={},after={},native_windows_complete=True,modules_complete=True)
        with self.assertRaisesRegex(ValueError,'summary differs'):
            validate_gui_resources(self.root,self.journal)

    def add_stages(self):
        self.journal['gui_workflow_stages'] = True
        snapshot = json.loads((self.root/'cycle-000001.json').read_text())
        stages = [dict(stage=name, elapsed_ms=index, snapshot=self.journal['gui_before_window'])
                  for index, name in enumerate(GUI_WORKFLOW_STAGES)]
        self.journal['observations'][0]['workflows'] = dict(gui_stages=stages)
        for stage in stages:
            (self.root/f"workflow-000001-{stage['stage']}.json").write_text(json.dumps(snapshot), encoding='utf-8')
        return stages

    def test_complete_stages(self):
        self.add_stages()
        self.assertEqual(len(validate_gui_resources(self.root,self.journal)),16)

    def test_missing_or_reordered_stages(self):
        stages = self.add_stages()
        for changed in [stages[:-1], stages[::-1], []]:
            self.journal['observations'][0]['workflows']['gui_stages'] = changed
            with self.assertRaisesRegex(ValueError,'operation order'):
                validate_gui_resources(self.root,self.journal)

    def test_bad_stage_time(self):
        stages = self.add_stages()
        for value in [-1, True, 0.5, '1']:
            stages[-1]['elapsed_ms'] = value
            with self.subTest(value=value), self.assertRaisesRegex(ValueError,'stage times'):
                validate_gui_resources(self.root,self.journal)

    def test_unrequested_stages(self):
        self.add_stages()
        self.journal['gui_workflow_stages'] = False
        with self.assertRaisesRegex(ValueError,'Unexpected GUI'):
            validate_gui_resources(self.root,self.journal)


class PlatformTests(unittest.TestCase):
    def journal(self, **changes):
        row = dict(window_visible=True, window_exposed=True, gdi_objects=12, user_objects=9)
        row.update(changes)
        return dict(qt_platform='windows', observations=[row])

    def test_native_window(self):
        validate_platform('windows', self.journal())

    def test_platform_substitution(self):
        with self.assertRaisesRegex(ValueError, 'platform differs'):
            validate_platform('offscreen', self.journal())

    def test_missing_actual_platform(self):
        with self.assertRaisesRegex(ValueError, 'platform differs'):
            validate_platform('windows', dict(observations=[]))

    def test_hidden_or_unexposed(self):
        for field in ['window_visible', 'window_exposed']:
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'visible and exposed'):
                validate_platform('windows', self.journal(**{field:False}))

    def test_empty_or_missing_native_resources(self):
        for rows in [[], self.journal(gdi_objects=0)['observations'], self.journal(user_objects=0)['observations']]:
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                validate_platform('windows', dict(qt_platform='windows', observations=rows))

    def test_offscreen_does_not_claim_native_resources(self):
        validate_platform('offscreen', dict(qt_platform='offscreen', observations=[dict(gdi_objects=0)]))


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
