"""CPU-only counterexamples for the development compatibility gate."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from validate_compatibility_gate import check_case, manifest_digest


class GateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.spec = dict(comparison_policy='exact_golden', reference_rgba_sha256='golden',
                         controls=[], boundaries=[])
        self.case = dict(status='repeat_stable', preflight=dict(exit_code=0, timed_out=False,
                         report=dict(completed=True, cancelled=False, gpu_validation='not_run',
                                     replay_proven=False, findings=[], errors=0, status='checked')),
                         native=[dict(exit_code=0, timed_out=False, runtime_dependency_audit='passed',
                                      report=dict(completed=True, rgba_sha256='golden'),
                                      artifacts={'frame.rgba': 'golden'}) for _ in range(2)],
                         controls=[], boundaries=[])

    def check(self, negative=None):
        check_case(self.case, self.spec, negative, self.directory, 2)

    def test_positive_and_known_variance(self):
        self.check()
        self.case['status'] = 'repeat_variable'
        with self.assertRaises(ValueError):
            self.check()
        self.spec.update(comparison_policy='known_variable', reference_rgba_sha256=None)
        self.check()

    def test_manifest_line_endings_only(self):
        path = self.directory / 'manifest.json'
        path.write_bytes(b'{\r\n  "id": 1\r\n}\r\n')
        expected = manifest_digest(path)
        path.write_bytes(b'{\n  "id": 1\n}\n')
        self.assertEqual(manifest_digest(path), expected)
        path.write_bytes(b'{\n  "id": 2\n}\n')
        self.assertNotEqual(manifest_digest(path), expected)

    def test_incomplete_or_false_preflight(self):
        original = copy.deepcopy(self.case)
        for key, value in [('completed', False), ('cancelled', True), ('gpu_validation', 'passed'),
                           ('replay_proven', True), ('errors', 1), ('status', 'blocked')]:
            with self.subTest(key=key):
                self.case = copy.deepcopy(original)
                self.case['preflight']['report'][key] = value
                with self.assertRaises(ValueError):
                    self.check()

    def test_native_crash_timeout_dependency_and_missing_image(self):
        original = copy.deepcopy(self.case)
        for key, value in [('exit_code', -1073741819), ('timed_out', True), ('launch_error', 'missing'),
                           ('runtime_dependency_audit', 'not_available'), ('artifacts', {}), ('report', {})]:
            with self.subTest(key=key):
                self.case = copy.deepcopy(original)
                self.case['native'][0][key] = value
                with self.assertRaises(ValueError):
                    self.check()
        self.case = original
        self.case['native'].pop()
        with self.assertRaises(ValueError):
            self.check()

    def negative(self):
        error = dict(kind='unresolved', event_id=4, resource_id=5, record_type=6)
        negative = dict(findings=[error], runtime_error='Event 4: unresolved resource 5')
        self.case['status'] = 'replay_failed'
        self.case['preflight']['exit_code'] = 2
        self.case['preflight']['report'].update(status='blocked', errors=1,
                                               findings=[dict(error, severity='error')])
        for index, run in enumerate(self.case['native'], 1):
            run.update(exit_code=1, report={}, runtime_dependency_audit='not_available')
            (self.directory / f'native-{index}.log').write_text(
                'progress 4 1 2\n' + json.dumps(dict(completed=False, error=negative['runtime_error'])) + '\n')
        return negative

    def test_located_negative(self):
        self.check(self.negative())

    def test_negative_cannot_be_a_crash_timeout_or_different_error(self):
        negative = self.negative()
        original = copy.deepcopy(self.case)
        for key, value in [('exit_code', -1073741819), ('timed_out', True), ('exit_code', 0)]:
            with self.subTest(key=key, value=value):
                self.case = copy.deepcopy(original)
                self.case['native'][0][key] = value
                with self.assertRaises(ValueError):
                    self.check(negative)
        self.case = original
        (self.directory / 'native-1.log').write_text('{"completed":false,"error":"unrelated failure"}')
        with self.assertRaises(ValueError):
            self.check(negative)

    def test_negative_location_and_count(self):
        negative = self.negative()
        self.case['preflight']['report']['findings'][0]['event_id'] = 7
        with self.assertRaises(ValueError):
            self.check(negative)
        negative = self.negative()
        self.case['native'].pop()
        with self.assertRaises(ValueError):
            self.check(negative)

    def test_missing_controls_and_boundaries(self):
        self.spec['controls'] = [dict(disable_event=4, repeat=2, expected_rgba_sha256='x')]
        with self.assertRaises(ValueError):
            self.check()
        self.spec['controls'] = []
        self.spec['boundaries'] = [dict(resource=5, event=4, expect_stable=True)]
        with self.assertRaises(ValueError):
            self.check()

    def test_boundary_hashes_cannot_trust_passed_flag(self):
        self.spec['boundaries'] = [dict(resource=5, event=4, expect_stable=True,
                                       expected_storage_sha256='bytes')]
        self.case['boundaries'] = [dict(resource=5, event=4, passed=True, complete=True,
                                       storage_sha256=['bytes', 'bytes'],
                                       runs=copy.deepcopy(self.case['native']))]
        self.check()
        self.case['boundaries'][0]['storage_sha256'][1] = 'wrong'
        with self.assertRaises(ValueError):
            self.check()

    def test_observed_images_cannot_trust_stable_status(self):
        self.spec.update(comparison_policy='observe', reference_rgba_sha256=None)
        self.case['native'][1]['report']['rgba_sha256'] = 'changed'
        self.case['native'][1]['artifacts']['frame.rgba'] = 'changed'
        with self.assertRaises(ValueError):
            self.check()


if __name__ == '__main__':
    unittest.main()
