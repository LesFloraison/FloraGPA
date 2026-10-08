"""CPU controls: validate declared corpus totals before starting GPU work."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from validate_compatibility_gate import check_registry_totals, manifest_digest


class RegistryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config = dict(expected_cases=4, expected_unique_captures=3, expected_rejections=1, suites=[])
        for index, rows in enumerate([[('a', 'A'), ('b', 'B')], [('c', 'A'), ('d', 'D')]]):
            path = self.root / f'{index}.json'
            path.write_text(json.dumps(dict(cases=[dict(id=name, sha256=sha) for name, sha in rows])))
            self.config['suites'].append(dict(id=str(index), manifest=path.name,
                manifest_sha256=manifest_digest(path), expected_rejections={'d': {}} if index else {}))

    def test_duplicate_capture_is_counted_once(self):
        check_registry_totals(self.config, self.root)

    def test_each_stale_total(self):
        for key in ['expected_cases', 'expected_unique_captures', 'expected_rejections']:
            changed = copy.deepcopy(self.config)
            changed[key] += 1
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, key):
                check_registry_totals(changed, self.root)

    def test_boolean_is_not_a_count(self):
        self.config['expected_rejections'] = True
        with self.assertRaisesRegex(ValueError, 'expected_rejections'):
            check_registry_totals(self.config, self.root)

    def test_manifest_mutation(self):
        (self.root / '0.json').write_text('{"cases":[]}')
        with self.assertRaisesRegex(ValueError, 'Changed corpus manifest'):
            check_registry_totals(self.config, self.root)

    def test_unknown_rejection(self):
        self.config['suites'][0]['expected_rejections']['missing'] = {}
        with self.assertRaisesRegex(ValueError, 'Unknown expected rejection'):
            check_registry_totals(self.config, self.root)


if __name__ == '__main__':
    unittest.main()
