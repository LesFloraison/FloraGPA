"""Instruction-pattern coverage, including false-positive boundaries."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from audit_original_execute_sites import scan_code


class ExecuteSiteTests(unittest.TestCase):
    def test_call_and_tail_jump(self):
        result=scan_code(bytes.fromhex('ff90d0010000ffa0d0010000'),0x1000)
        self.assertEqual([r['address'] for r in result['candidates']],[0x1000,0x1006])
        self.assertTrue(all(r['direct_indirect_transfer'] for r in result['candidates']))

    def test_register_load_is_retained_but_not_classified_as_call(self):
        result=scan_code(bytes.fromhex('488b80d0010000ffd0'),0)
        self.assertEqual(len(result['candidates']),1)
        self.assertFalse(result['candidates'][0]['direct_indirect_transfer'])

    def test_stack_read_is_marked(self):
        result=scan_code(bytes.fromhex('488b8424d0010000'),0)
        self.assertTrue(result['candidates'][0]['stack_base'])

    def test_other_slot_address_and_write_are_not_calls(self):
        for raw in ['ff90c8010000','488d80d0010000','488980d0010000','488b05d0010000']:
            with self.subTest(raw=raw):self.assertEqual(scan_code(bytes.fromhex(raw),0)['candidates'],[])

    def test_incomplete_instruction_is_explicit_skipped_data(self):
        result=scan_code(bytes.fromhex('c3488b'),0x1000)
        self.assertEqual(result['accounted_bytes'],3)
        self.assertTrue(result['skipped_data'])
        self.assertEqual(result['candidates'],[])


if __name__=='__main__':unittest.main()
