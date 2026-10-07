"""CPU-only negative controls for address/heap evidence aggregation."""
import copy
import unittest

from analyze_process_memory import validate


def fixture():
    rows = [dict(base=4096, allocation_base=4096, bytes=4096, state=0x1000,
                 type=0x20000, protect=4, heap_busy_bytes=512, heap_busy_blocks=1),
            dict(base=8192, allocation_base=4096, bytes=4096, state=0x2000,
                 type=0x20000, protect=0, heap_busy_bytes=0, heap_busy_blocks=0),
            dict(base=16384, allocation_base=16384, bytes=4096, state=0x1000,
                 type=0x1000000, protect=2, heap_busy_bytes=0, heap_busy_blocks=0)]
    groups = [dict(base=4096, committed=4096, reserved=4096, private_committed=4096,
                   heap_busy_bytes=512, heap_busy_blocks=1),
              dict(base=16384, committed=4096, reserved=0, private_committed=0,
                   heap_busy_bytes=0, heap_busy_blocks=0)]
    summary = dict(address_walk_complete=True, heap_walk_complete=True, unmapped_heap_blocks=0,
                   private_committed=4096, mapped_committed=0, image_committed=4096,
                   heap_busy_bytes=512, heap_busy_blocks=1,
                   private_committed_in_heap_allocations=4096,
                   private_committed_in_other_allocations=0)
    return dict(schema='FloraGPA process address and heap snapshot 1', summary=summary,
                allocations=groups, regions=rows)


class MemoryAnalysisTests(unittest.TestCase):
    def test_valid_mixed_commit_and_reservation(self):
        sample = fixture()
        self.assertEqual(validate(sample), sample['summary'])

    def test_incomplete_or_unassociated(self):
        for key, value in [('address_walk_complete', False), ('heap_walk_complete', False),
                           ('unmapped_heap_blocks', 1)]:
            with self.subTest(key=key):
                sample = fixture()
                sample['summary'][key] = value
                with self.assertRaises(ValueError):
                    validate(sample)

    def test_incorrect_summary(self):
        for key in ['private_committed', 'mapped_committed', 'image_committed',
                    'heap_busy_bytes', 'heap_busy_blocks',
                    'private_committed_in_heap_allocations', 'private_committed_in_other_allocations']:
            with self.subTest(key=key):
                sample = fixture()
                sample['summary'][key] += 1
                with self.assertRaises(ValueError):
                    validate(sample)

    def test_allocation_disagrees_with_regions(self):
        sample = fixture()
        sample['allocations'][0]['private_committed'] += 4096
        with self.assertRaisesRegex(ValueError, 'Allocation totals'):
            validate(sample)

    def test_invalid_region(self):
        for key, value in [('base', 4095), ('bytes', 0), ('allocation_base', 12288),
                           ('state', 0x10000), ('type', 0), ('heap_busy_blocks', -1),
                           ('heap_busy_bytes', 1)]:
            with self.subTest(key=key):
                sample = fixture()
                sample['regions'][1][key] = value
                with self.assertRaises(ValueError):
                    validate(sample)

    def test_duplicated_allocation(self):
        sample = fixture()
        sample['allocations'].append(copy.deepcopy(sample['allocations'][0]))
        with self.assertRaises(ValueError):
            validate(sample)


if __name__ == '__main__':
    unittest.main()
