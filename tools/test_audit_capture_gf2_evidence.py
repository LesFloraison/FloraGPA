"""CPU-only development tests; not a packaged runtime dependency."""
import json
from pathlib import Path
import struct
import tempfile
import unittest

import audit_capture_gf2_evidence as subject


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def frame(self):
        header = bytearray(0x128)
        header[:4] = b'IGPA'
        struct.pack_into('<III', header, 4, 0x128, 3, 2)
        resource = bytearray(68)
        struct.pack_into('<QQ7I', resource, 0, 2**64-2, 3, 1, 1, 1, 1, 28, 1, 0)
        struct.pack_into('<Q', resource, 60, 2)
        blob = struct.pack('<I', 4) + bytes((10, 20, 30, 255))
        table = len(header) + len(resource) + len(blob)
        struct.pack_into('<Q', header, 244, table)
        data = header + resource + blob
        data += struct.pack('<QQIBBH', 1, len(header), len(resource), 0, 5, 0x87)
        data += struct.pack('<QQIBBH', 2, len(header)+len(resource), len(blob), 0, 9, 1)
        return data, table

    def write(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        return path

    def test_dedicated_resource(self):
        data, _ = self.frame()
        self.assertEqual(subject.reference(self.write('frame', data)), (1, 1, bytes((10, 20, 30, 255))))

    def test_invalid_container(self):
        original, table = self.frame()
        mutations = {
            'version': (8, '<I', 4),
            'index_overflow': (244, '<Q', 2**64-1),
            'duplicate_id': (table+24, '<Q', 1),
            'record_in_index': (table+8, '<Q', table),
            'compressed': (table+20, '<B', 1),
            'missing_blob': (0x128+60, '<Q', 999),
            'blob_size': (0x128+68, '<I', 8),
            'wrong_blob_kind': (table+24+22, '<H', 2),
            'wrong_format': (0x128+32, '<I', 29),
            'oversized_image': (0x128+16, '<I', 2**32-1),
            'no_reference': (0x128, '<Q', 1),
        }
        for name, (offset, fmt, value) in mutations.items():
            with self.subTest(name=name):
                data = original.copy()
                struct.pack_into(fmt, data, offset, value)
                with self.assertRaises(ValueError):
                    subject.reference(self.write(name, data))
        for length in (0, 4, 0x127, table, len(original)-1):
            with self.subTest(length=length), self.assertRaises(ValueError):
                subject.reference(self.write('truncated', original[:length]))

    def image(self, top=True):
        header = bytearray(18)
        header[2] = 2
        struct.pack_into('<HHBB', header, 12, 1, 2, 32, 0x28 if top else 8)
        rows = [bytes((30, 20, 10, 255)), bytes((60, 50, 40, 128))]
        return header + b''.join(rows if top else reversed(rows))

    def test_tga_channel_and_row_order(self):
        expected = (1, 2, bytes((10, 20, 30, 255, 40, 50, 60, 128)))
        for top in (True, False):
            self.assertEqual(subject.tga(self.write('image', self.image(top))), expected)

    def test_invalid_tga(self):
        for name, offset, value in (('compressed', 2, 10), ('palette', 1, 1),
                                    ('depth', 16, 24), ('right_origin', 17, 0x38), ('interleaved', 17, 0x68)):
            with self.subTest(name=name), self.assertRaises(ValueError):
                data = self.image()
                data[offset] = value
                subject.tga(self.write(name, data))
        with self.assertRaises(ValueError):
            subject.tga(self.write('short', self.image()[:-1]))

    def test_ambiguous_output_refused(self):
        with self.assertRaises(ValueError):
            subject.new_image(self.root)
        self.write('framebuffer_new.tga', self.image())
        subject.new_image(self.root)
        self.write('framebuffer_new_1.tga', self.image())
        with self.assertRaises(ValueError):
            subject.new_image(self.root)

    def test_exact_difference(self):
        self.assertEqual(subject.differences(bytes(8), bytes((1, 2, 0, 0, 0, 0, 3, 0))),
                         dict(pixels=2, channels=3, maximum=3))
        with self.assertRaises(ValueError):
            subject.differences(bytes(8), bytes(4))

    def test_execution_claims(self):
        binary = self.write('dummy-binary', b'unit-test')
        audit = subject.Audit(self.root, binary, binary, binary)
        directory = self.root/'flora'
        directory.mkdir()
        (directory/'frame.rgba').write_bytes(bytes(4))
        report = dict(completed=True, reference_pixels_used=False,
                      adapter='Intel(R) Iris(R) Xe Graphics', counts=dict(DrawIndexed=1, Present=1),
                      capture=str(binary), width=1, height=1)

        def check(value, negative=False):
            (directory/'report.json').write_text(json.dumps(value), encoding='utf-8')
            audit.player(directory, False, negative, binary)
            return audit.checks[-1]['passed']

        self.assertTrue(check(report))
        for patch in (dict(reference_pixels_used=True), dict(completed=False),
                      dict(counts=dict(Present=1)), dict(capture=str(self.root/'different-frame')),
                      dict(counts=dict(DrawIndexed=1)), dict(adapter='Other GPU')):
            with self.subTest(patch=patch):
                self.assertFalse(check(report | patch))
        self.assertFalse(check(report, True))
        self.assertTrue(check(report | dict(counts=dict(Present=1)), True))
        self.assertTrue(audit.inputs[str(directory/'frame.rgba')])

    def test_original_execution_claims(self):
        binary = self.write('dummy-binary', b'unit-test')
        audit = subject.Audit(self.root, binary, binary, binary)
        directory = self.root/'original'
        directory.mkdir()
        (directory/'framebuffer_new.tga').write_bytes(self.image())
        native = dict(negative_control_suppress_draws=False, actual_context_adapters=[dict(vendor_id=0x8086)],
                      d3d11_calls=dict(DrawIndexed=1))
        report = dict(closed=True, open_status=0, playback_status=0, callbacks=[],
                      player_sha256=subject.PLAYER_SHA, audit=native)

        def check(value, negative=False):
            (directory/'result.json').write_text(json.dumps(value), encoding='utf-8')
            audit.player(directory, True, negative, binary)
            return audit.checks[-1]['passed']

        self.assertTrue(check(report))
        for patch in (dict(closed=False), dict(playback_status=1), dict(callbacks=['error']),
                      dict(player_sha256='wrong-player'), dict(audit=native | dict(d3d11_calls={})),
                      dict(audit=native | dict(actual_context_adapters=[dict(vendor_id=0x10de)]))):
            with self.subTest(patch=patch):
                self.assertFalse(check(report | patch))
        self.assertFalse(check(report, True))
        negative = native | dict(negative_control_suppress_draws=True, d3d11_calls=dict(suppressed_DrawIndexed=1))
        self.assertTrue(check(report | dict(audit=negative), True))


if __name__ == '__main__':
    unittest.main()
