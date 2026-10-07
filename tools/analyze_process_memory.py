"""Validate and compare test-only address/heap metadata; never reads page contents.

An address is not a persistent allocation identity. Heap association is based on
the start of observed busy blocks, not ownership of every committed page.
"""
import argparse
import hashlib
import json
from pathlib import Path


NAMES = ['warmup', 'baseline', 'before_clear', 'after_history_clear',
         'after_log_clear', 'after_pixmap_cache_clear']
FIELDS = ['private_bytes_before', 'private_bytes_after', 'private_committed',
          'private_committed_in_heap_allocations',
          'private_committed_in_other_allocations', 'heap_busy_bytes',
          'heap_busy_blocks', 'mapped_committed', 'image_committed']


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate(snapshot):
    require(snapshot['schema'] == 'FloraGPA process address and heap snapshot 1', 'Unknown schema')
    summary = snapshot['summary']
    require(summary['address_walk_complete'] and summary['heap_walk_complete'], 'Incomplete walk')
    # These diagnostic runs require complete block association. A nonzero value
    # is possible for a non-atomic observation, but cannot support this comparison.
    require(summary['unmapped_heap_blocks'] == 0, 'Unassociated heap blocks')
    groups = {}
    committed = {0x20000: 0, 0x40000: 0, 0x1000000: 0}
    end = 0
    for row in snapshot['regions']:
        base, size = row['base'], row['bytes']
        require(isinstance(base, int) and isinstance(size, int) and base >= end and size > 0,
                'Unordered, overlapping or empty region')
        end = base + size
        require(row['allocation_base'] <= base, 'Allocation starts after region')
        require(row['state'] in [0x1000, 0x2000] and row['type'] in committed, 'Unexpected region kind')
        require(row['heap_busy_bytes'] >= 0 and row['heap_busy_blocks'] >= 0, 'Negative heap count')
        if row['state'] != 0x1000:
            require(row['heap_busy_bytes'] == row['heap_busy_blocks'] == 0, 'Busy block in reserved region')
        a = groups.setdefault(row['allocation_base'], dict(committed=0, reserved=0,
                             private_committed=0, heap_busy_bytes=0, heap_busy_blocks=0))
        if row['state'] == 0x1000:
            a['committed'] += size
            committed[row['type']] += size
            if row['type'] == 0x20000:
                a['private_committed'] += size
        else:
            a['reserved'] += size
        a['heap_busy_bytes'] += row['heap_busy_bytes']
        a['heap_busy_blocks'] += row['heap_busy_blocks']
    expected = [dict(base=base, **group) for base, group in sorted(groups.items())]
    require(expected == snapshot['allocations'], 'Allocation totals disagree with raw regions')
    for name, kind in [('private_committed', 0x20000), ('mapped_committed', 0x40000),
                       ('image_committed', 0x1000000)]:
        require(summary[name] == committed[kind], f'Incorrect {name}')
    for name in ['heap_busy_bytes', 'heap_busy_blocks']:
        require(summary[name] == sum(a[name] for a in groups.values()), f'Incorrect {name}')
    associated = sum(a['private_committed'] for a in groups.values() if a['heap_busy_blocks'])
    require(summary['private_committed_in_heap_allocations'] == associated, 'Incorrect heap association total')
    require(summary['private_committed_in_other_allocations'] == committed[0x20000] - associated,
            'Incorrect other allocation total')
    return summary


def analyze(root):
    root = Path(root)
    runner = json.loads((root / 'validation.json').read_text(encoding='utf-8'))
    require(runner['completed'] and runner['passed'] and runner['memory_maps'], 'Runner not accepted')
    journal = json.loads((root / 'journal.json').read_text(encoding='utf-8'))
    files, snapshots = {}, {}
    for name in NAMES:
        relative = f'memory-maps/{name}.json'
        raw = (root / relative).read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        require(digest == runner['memory_map_files'][relative], f'Changed snapshot: {name}')
        snapshot = json.loads(raw)
        require(validate(snapshot) == journal['memory_maps'][name], 'Journal summary mismatch')
        files[relative] = digest
        snapshots[name] = snapshot
    summaries = {name: snapshot['summary'] for name, snapshot in snapshots.items()}
    comparisons = {}
    for first, last in [('baseline', 'before_clear'), ('before_clear', 'after_history_clear'),
                        ('after_history_clear', 'after_log_clear'),
                        ('after_log_clear', 'after_pixmap_cache_clear'),
                        ('baseline', 'after_pixmap_cache_clear')]:
        deltas = {key: summaries[last][key] - summaries[first][key] for key in FIELDS}
        old = {a['base']: a for a in snapshots[first]['allocations']}
        new = {a['base']: a for a in snapshots[last]['allocations']}
        changed = []
        for base in old.keys() | new.keys():
            a, b = old.get(base, {}), new.get(base, {})
            delta = {key: b.get(key, 0) - a.get(key, 0)
                     for key in ['private_committed', 'heap_busy_bytes', 'heap_busy_blocks']}
            if any(delta.values()):
                changed.append(dict(address=base, delta=delta,
                                    observed_before=bool(a), observed_after=bool(b)))
        changed.sort(key=lambda row: (-abs(row['delta']['private_committed']), row['address']))
        comparisons[f'{first}_to_{last}'] = dict(delta=deltas, largest_address_changes=changed[:15])
    return dict(schema='FloraGPA process memory comparison 1',
                cycles=runner['cycles'], elapsed_ms=runner['elapsed_ms'],
                files=files, summaries=summaries, comparisons=comparisons,
                scope='Sequential metadata from one instrumented test process. Address reuse and copy-on-write are not resolved. Serialization affects subsequent samples. Heap-associated commit is not live-object size. This is not leak-free certification.')


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('run', type=Path, help='Accepted validate_recovery_soak.py output directory')
    args = parser.parse_args()
    print(json.dumps(analyze(args.run), indent=2))


if __name__ == '__main__':
    main()
