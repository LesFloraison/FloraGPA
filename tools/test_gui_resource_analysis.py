"""CPU controls for retained GUI evidence and correlation summaries."""
import json
from pathlib import Path
import tempfile
import unittest

from analyze_gui_resources import summarize
from validate_recovery_soak import digest, validate_gui_resources


class AnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='Flora GUI analysis ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.gui = self.root / 'gui-resources'
        self.gui.mkdir()
        self.journal = dict(completed=True, phase='complete', observations=[], retention_control={})
        names = ['before_window', 'cycle-000001', 'control-before',
                 'control-after_test_history_clear', 'control-after_log_clear',
                 'control-after_pixmap_cache_clear', 'after_window_destroy']
        for index, name in enumerate(names):
            counts = dict(gdi_objects=2 if index == 0 else 6, handles=10, user_objects=1)
            row = dict(schema='FloraGPA GUI resource snapshot 1', process_id=17, qt_platform='windows',
                       before=counts, after=counts, modules_complete=True, native_windows_complete=True,
                       modules=[dict(name='app.exe', path='C:/app/app.exe')],
                       native_top_windows=[], qt_top_widgets=[], qt_windows=[])
            if index == 1:
                row['modules'].append(dict(name='extra.dll', path='C:/app/extra.dll'))
                row['qt_top_widgets'] = [{'class': 'Window'}, {'class': 'Window'}]
            self.write(self.gui / (name + '.json'), row)
            summary = {k: row[k] for k in ['before', 'after', 'modules_complete', 'native_windows_complete']}
            if name == 'before_window':
                self.journal['gui_before_window'] = summary
            elif name == 'after_window_destroy':
                self.journal['gui_after_window_destroy'] = summary
            elif name.startswith('cycle-'):
                self.journal['observations'].append(dict(gui_snapshot=summary))
            else:
                self.journal['retention_control'][name.removeprefix('control-')] = dict(gui_snapshot=summary)
        self.write(self.root / 'journal.json', self.journal)
        self.report = dict(completed=True, passed=True, gui_resources=True, qt_platform='windows',
                           journal_sha256=digest(self.root / 'journal.json'),
                           gui_resource_files=validate_gui_resources(self.gui, self.journal))
        self.write(self.root / 'validation.json', self.report)

    def write(self, path, value):
        path.write_text(json.dumps(value), encoding='utf-8')

    def test_order_counts_and_correlations(self):
        before = {p: digest(p) for p in self.root.rglob('*') if p.is_file()}
        rows = summarize(self.root)['observations']
        self.assertEqual(len(rows), 7)
        self.assertEqual(rows[-1]['snapshot'], 'after_window_destroy.json')
        self.assertEqual(rows[1]['since_previous']['counts'], dict(gdi_objects=4))
        self.assertEqual(rows[1]['since_previous']['qt_top_widgets'], dict(Window=2))
        self.assertEqual(rows[2]['since_previous']['qt_top_widgets'], dict(Window=-2))
        self.assertEqual(rows[1]['since_previous']['modules_added'], ['C:/app/extra.dll'])
        self.assertEqual(rows[2]['since_previous']['modules_removed'], ['C:/app/extra.dll'])
        self.assertTrue(all(not row['during_probe_delta'] for row in rows))
        self.assertEqual(before, {p: digest(p) for p in self.root.rglob('*') if p.is_file()})

    def test_unaccepted_run(self):
        for key, value in [('completed', False), ('passed', False), ('gui_resources', False),
                           ('qt_platform', 'offscreen')]:
            with self.subTest(key=key):
                self.write(self.root / 'validation.json', self.report | {key: value})
                with self.assertRaisesRegex(ValueError, 'accepted native'):
                    summarize(self.root)

    def test_changed_journal(self):
        self.write(self.root / 'journal.json', self.journal | dict(changed=True))
        with self.assertRaisesRegex(ValueError, 'Journal identity'):
            summarize(self.root)

    def test_changed_metadata_without_changed_counts(self):
        path = self.gui / 'cycle-000001.json'
        row = json.loads(path.read_text())
        row['qt_top_widgets'] = []
        self.write(path, row)
        with self.assertRaisesRegex(ValueError, 'snapshot identity'):
            summarize(self.root)

    def test_missing_snapshot(self):
        (self.gui / 'before_window.json').unlink()
        with self.assertRaisesRegex(ValueError, 'inventory differs'):
            summarize(self.root)


if __name__ == '__main__':
    unittest.main()
