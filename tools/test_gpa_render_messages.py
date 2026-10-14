"""CPU checks using descriptor words observed in original GPA exports."""
import unittest
from inspect_gpa_render_messages import inspect


ORIGINAL = '(f1.1) sendc.rc (8|M0) null r29 null 0x0 0x10030201 {@1,$15}'
FINAL = '(f0.0) sendc.rc (8|M0) null r124 null 0x0 0x08031401 {EOT,@1}'


class RenderMessages(unittest.TestCase):
    def test_observed_original(self):
        row = inspect(ORIGINAL)['render_writes'][0]
        self.assertEqual((row['binding_table_index'], row['subtype'], row['predicate']),
                         (1, 'lo8ds', '(f1.1)'))
        self.assertFalse(row['last_render_target'])
        self.assertFalse(row['end_of_thread'])

    def test_observed_modified_final(self):
        row = inspect(FINAL)['render_writes'][0]
        self.assertEqual((row['binding_table_index'], row['subtype']), (1, 'simd8'))
        self.assertTrue(row['last_render_target'])
        self.assertTrue(row['end_of_thread'])

    def test_comments_do_not_create_instructions(self):
        rows = inspect('// '+FINAL+'\n'+ORIGINAL+' // '+FINAL)['render_writes']
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['line'], 2)

    def test_unverified_forms_fail(self):
        for source in [ORIGINAL.replace('(8|M0)', '(16|M0)'),
                       ORIGINAL.replace('0x10030201', 'a0.0'),
                       ORIGINAL.replace('null r29', 'r0 r29'),
                       ORIGINAL.replace('0x10030201', '0x110030201')]:
            with self.subTest(source=source), self.assertRaises(ValueError):
                inspect(source)

    def test_unverified_descriptor_kinds_fail(self):
        for descriptor in ['0x10034201', '0x50030201', '0x10030701']:
            with self.subTest(descriptor=descriptor), self.assertRaises(ValueError):
                inspect(ORIGINAL.replace('0x10030201', descriptor))

    def test_missing_or_partially_bad_stream_fails(self):
        for source in ['', '// '+ORIGINAL, ORIGINAL+'\nsendc.rc corrupt']:
            with self.subTest(source=source), self.assertRaises(ValueError):
                inspect(source)


if __name__ == '__main__':
    unittest.main()
