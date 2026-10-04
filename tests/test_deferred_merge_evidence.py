"""CPU negative controls for capture-fidelity evidence, without installed GPA."""
import copy
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from validate_deferred_merge_evidence import check_frame


class MergeEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def fixture(self, lost=False):
        expected = [(60, 3, 2, 3), (2817, 6, 5, 3), (126996, 9, 11, 3)]
        actual = [(2, 3, 2, 0), (5, 6, 5, 0), (11, 9, 11, 0)] if lost else expected
        frame = dict(frame=6, finish_state_failures=0, execute_state_failures=0,
                     merge_state_failures=0, parent_finish_state_failures=0,
                     image_verified=not lost, steps=[])
        for step in range(3):
            frame['steps'].append(dict(step=step, actual=list(actual[step]),
                                       expected=list(expected[step]), verified=not lost))
            (self.root / f'frame-6-step-{step}.bin').write_bytes(struct.pack('<4I', *actual[step]))
        (self.root / 'frame-6.rgba').write_bytes(bytes([255, 0, 0, 255] if lost else [0, 255, 0, 255]) * 64)
        (self.root / 'frame-6-baseline.bin').write_bytes(struct.pack('<4I', 101, 103, 107, 109))
        return frame

    def test_native_and_immutable_capture_require_correct_workload(self):
        frame = self.fixture()
        check_frame(self.root, frame, 0, 'native')
        check_frame(self.root, frame, 32, 'captured')

    def test_known_capture_loss_is_separate_from_native_success(self):
        frame = self.fixture(lost=True)
        check_frame(self.root, frame, 0, 'captured')
        for kind, mode in [('native', 0), ('captured', 32)]:
            with self.subTest(kind=kind, mode=mode), self.assertRaises(ValueError):
                check_frame(self.root, frame, mode, kind)

    def test_cannot_replace_cpu_oracle_with_captured_bad_result(self):
        frame = self.fixture(lost=True)
        frame['steps'][0]['expected'] = frame['steps'][0]['actual']
        with self.assertRaises(ValueError):
            check_frame(self.root, frame, 0, 'captured')

    def test_cannot_mark_bad_capture_as_verified(self):
        frame = self.fixture(lost=True)
        frame['steps'][1]['verified'] = True
        with self.assertRaises(ValueError):
            check_frame(self.root, frame, 0, 'captured')

    def test_all_four_words_and_file_lengths_are_checked(self):
        frame = self.fixture()
        for raw in [struct.pack('<4I', 126996, 8, 11, 3), struct.pack('<3I', 126996, 9, 11)]:
            with self.subTest(raw=raw):
                (self.root / 'frame-6-step-2.bin').write_bytes(raw)
                with self.assertRaises(ValueError):
                    check_frame(self.root, frame, 0, 'native')

    def test_image_mismatch_is_not_hidden_by_correct_resources(self):
        frame = self.fixture()
        (self.root / 'frame-6.rgba').write_bytes(bytes([255, 0, 0, 255]) * 64)
        with self.assertRaises(ValueError):
            check_frame(self.root, frame, 0, 'native')

    def test_state_failures_are_localized_to_capture_frame_and_flags(self):
        frame = self.fixture(lost=True)
        frame.update(execute_state_failures=3, merge_state_failures=3, parent_finish_state_failures=1)
        check_frame(self.root, frame, 28, 'captured')
        for mode in [20, 24, 30]:
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                check_frame(self.root, frame, mode, 'captured')
        other = copy.deepcopy(frame)
        other['frame'] = 5
        with self.assertRaises(ValueError):
            check_frame(self.root, other, 28, 'captured')

    def test_missing_step_cannot_be_accepted(self):
        frame = self.fixture()
        frame['steps'].pop()
        with self.assertRaises(ValueError):
            check_frame(self.root, frame, 0, 'native')


if __name__ == '__main__':
    unittest.main()
