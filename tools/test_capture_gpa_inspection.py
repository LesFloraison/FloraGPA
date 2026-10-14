"""CPU refusal tests using real retained exports, without rerunning GPA."""
import argparse
import copy
from pathlib import Path
import unittest

import validate_capture_gpa_inspection as subject


class Refusals(unittest.TestCase):
    def setUp(self):
        self.original = copy.deepcopy(self.baseline)

    def validate(self):
        return subject.validate_frame(self.original, self.inventory, self.selection, self.launch_root, 1)

    def binding(self, graphics=False):
        entry = next(x for x in self.selection['events'] if (x['kind'] != 53) == graphics)
        call = subject.match_actions(self.original, self.inventory)[entry['event']]
        return self.original['bindings'][str(call['id'])]

    def test_unmodified_inspection(self):
        self.assertEqual(len(self.validate()['selected']), 3)

    def test_changed_draw_argument(self):
        call = next(c for c in self.original['calls'] if c['name'] == 'DrawIndexed')
        call['arguments'][0]['value'] += 1
        with self.assertRaisesRegex(ValueError, 'arguments differ'):
            self.validate()

    def test_dropped_draw(self):
        call = next(c for c in self.original['calls'] if c['name'] == 'DrawIndexed')
        self.original['calls'].remove(call)
        with self.assertRaisesRegex(ValueError, 'counts differ'):
            self.validate()

    def test_changed_shader_instruction(self):
        binding = self.binding()
        program = self.original['programs'][str(binding['execution']['program']['id'])]
        program['compute']['dxbc'] += '\nret\n'
        with self.assertRaisesRegex(ValueError, 'Shader differs'):
            self.validate()

    def test_missing_resource_usage(self):
        binding = self.binding()
        resource = binding['outputs'][0]
        for view in self.original['resources'][str(resource['resource_id'])]:
            view['usages'] = []
        with self.assertRaisesRegex(ValueError, 'event usage missing'):
            self.validate()

    def test_changed_viewport(self):
        binding = self.binding(True)
        binding['execution']['states']['Viewport 0']['WIDTH'] += 1
        with self.assertRaisesRegex(ValueError, 'graphics states differ'):
            self.validate()

    def test_missing_grouping(self):
        del self.original['groupings']['Debug Region']
        with self.assertRaisesRegex(ValueError, 'Missing grouping'):
            self.validate()

    def test_binding_lookup_error(self):
        self.binding()['ok'] = False
        with self.assertRaisesRegex(ValueError, 'binding lookup failed'):
            self.validate()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--export', type=Path, required=True)
    args = parser.parse_args()
    Refusals.launch_root = args.evidence/'launch-1'
    Refusals.baseline = subject.read(args.export)
    Refusals.inventory = subject.read(Refusals.launch_root/'inventory-1/report.json')
    Refusals.selection = next(x for x in subject.read(Refusals.launch_root/'inspection-selection.json') if x['index'] == 1)
    unittest.main(argv=['test_capture_gpa_inspection'],verbosity=2)
