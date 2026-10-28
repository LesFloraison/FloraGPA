"""Check class layout evidence while retaining every original GPU mismatch."""
import argparse
import copy
import itertools
import json
from pathlib import Path
from validate_texture_precision import instructions, require

FIXTURES = ('wide-array', 'multi-array', 'wide-multi-array')
VENDORS = (0, 4318, 32902)


def validate(report, native, binary):
    fixture, vendor, mode = native['fixture'], native['vendor'], native['mode']
    require(fixture in FIXTURES and vendor in VENDORS and type(mode) is int and mode in range(4), 'Native layout identity')
    wide, multiple = 'wide' in fixture, 'multi' in fixture
    slots = 4 if multiple else 2
    require(report['schema'] == 1 and report['probe'] == 'original_dxbc_interfaces'
            and report['fixture'] == fixture and report['vendor'] == (vendor or 5140)
            and report['warp'] is (vendor == 0) and report['slots'] == slots
            and report['feature_level'] == 0xb100 and report['device'] > 0
            and report['luid_low'] | report['luid_high'], 'Original adapter and layout')
    require(report['selected_base'] == 0 and report['extra_base'] == (2 if multiple else None),
            'Independently reflected interface bases')
    require(report['output_slot'] == 3 and report['seed'] == 0xdeadbeef
            and all(d['severity'] > 1 for d in report['diagnostics']), 'Original execution contract')
    words = [component for i in range(8 if wide else 4) for component in ((i + 1) * 10, 0, 0, 0)]
    require(report['constant_words'] == words, 'Original constant storage')
    require([c['mode'] for c in report['cases']] == list(range(4)), 'Complete original binding matrix')
    header, rows = instructions(binary.hex())
    declarations = [r for r in rows if r[0] & 2047 == 146]
    require(header == 0x50050 and len(declarations) == (2 if multiple else 1), 'CS interface declarations')
    require([r[1] for r in declarations] == ([0, 2] if multiple else [0])
            and all(r[0] & 0x800 and r[3] >> 16 == 2 for r in declarations), 'Dynamic interface arrays')
    require(sum(r[0] & 2047 == 120 for r in rows) == 2, 'Both interface calls remain in original DXBC')
    a, b = (((70, 110), (30, 150), (90, 50), (50, 70)) if wide else
            ((20, 30), (10, 40), (40, 20), (20, 30)))[mode]
    if multiple:
        expected = [i * b + i + b * 3 if mode & 1 else i + a + (i ^ a) for i in range(4)]
    else:
        expected = [i * b + (i ^ a) if mode & 1 else i + a + i + b * 3 for i in range(4)]
    item = report['cases'][mode]
    require(item['expected'] == expected and native['authored'] == expected
            and native['native'] == expected and native['native_matches_authored'] is True,
            'Native result must match independently authored arithmetic')
    require(len(item['actual']) == 4 and native['original_gpu'] == item['actual']
            and native['native_matches_original_gpu'] is (expected == item['actual']), 'Honest original GPU comparison')
    require(report['passed'] is all(c['actual'] == c['expected'] for c in report['cases']), 'Original outcome')
    require(len(native['bindings']) == slots and len(item['bindings']) == slots, 'Every bound slot')
    for slot, binding in enumerate(native['bindings']):
        original = item['bindings'][slot]
        is_a = slot in (0, 3)
        created = mode >= 2
        desc = original['descriptor']
        require(len(desc) == 8 and desc[7] == int(created), 'Observed creation mode')
        require(binding['slot'] == slot and binding['object'] == 100 + slot
                and binding['instance_name'] == original['name']
                and binding['type_name'] == original['type'] and binding['observed_descriptor'] == desc,
                'Native provenance retains actual observed descriptor')
        require(original['name'] == ('' if created else 'first' if is_a else 'second')
                and original['type'] == (('A' if is_a else 'B') if created else ''),
                'Observed class identity')
        if created:
            pointer = [2 if is_a else 3, (3 if is_a else 1) if mode == 2 else (1 if is_a else 2),
                       5 if is_a else 9, 7 if is_a else 11]
        else:
            index = (1 if is_a else 0) if mode == 0 else (0 if is_a else 1)
            require(desc[1] == index, 'Named array element')
            pointer = [1, ((0 if is_a else 2) + index) * (2 if wide else 1), 0, 0]
        require(binding['this_bits'] == pointer and binding['this_valid'] == (15 if created else 3),
                'Resolved constant stride and unknown unused resources')
        require(binding['shader_type'] == int(is_a)
                and binding['function_table'] == int(is_a) + (2 if slot >= 2 else 0),
                'Shader-local type and per-interface function table')
    return sum(a == b for a, b in zip(expected, item['actual']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    matched = corruptions = 0
    for vendor, fixture in itertools.product(VENDORS, FIXTURES):
        key = f'layout-{vendor}-{fixture}'
        directory = args.directory / key
        report = json.loads((directory / 'result.json').read_text())
        binary = (directory / 'shader.dxbc').read_bytes()
        for mode in range(4):
            native = json.loads((args.directory / f'{key}-native-{mode}.json').read_text())
            require((native['vendor'], native['fixture'], native['mode']) == (vendor, fixture, mode),
                    'Unique requested native layout configuration')
            matched += validate(report, native, binary)
            for field in ('native_matches_authored', 'native_matches_original_gpu'):
                changed = copy.deepcopy(native)
                changed[field] = not changed[field]
                try:
                    validate(report, changed, binary)
                except (ValueError, KeyError, IndexError):
                    corruptions += 1
                else:
                    raise ValueError('Accepted corrupted native comparison')
            for field in ('this_bits', 'this_valid', 'function_table'):
                changed = copy.deepcopy(native)
                if field == 'this_bits': changed['bindings'][0][field][1] += 1
                else: changed['bindings'][0][field] ^= 1
                try:
                    validate(report, changed, binary)
                except (ValueError, KeyError, IndexError):
                    corruptions += 1
                else:
                    raise ValueError('Accepted corrupted native binding')
    print(json.dumps({'evidence_valid': True, 'configurations': 36, 'native_authored_matching_words': 144,
                      'original_gpu_matching_words': matched, 'original_gpu_mismatching_words': 144 - matched,
                      'corruption_rejections': corruptions, 'acceptance_passed': matched == 144}, indent=2))
    return int(matched != 144)


if __name__ == '__main__':
    raise SystemExit(main())
