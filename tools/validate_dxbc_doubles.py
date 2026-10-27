"""Validate retained native double instruction results against independent GPU observations."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path
import re
import struct

from validate_typed_load_precision import require, shader_rows


OPCODES = (*range(191, 203), *range(210, 213), *range(214, 218))
SCENARIOS = ('arithmetic', 'minmax', 'comparison', 'tofloat', 'fromfloat', 'toint',
             'touint', 'fromint', 'fromuint', 'division', 'conditional', 'loop')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def instruction(opcode, *operands):
    words = [opcode]
    for operand in operands:
        words.extend(operand)
    words[0] |= len(words) << 24
    return tuple(words)


def raw_rows(report):
    opcode, form, stage = (report[key] for key in ('opcode', 'form', 'stage'))
    require(opcode in OPCODES and form in range(4) and stage in (0, 1, 5), 'Wrong raw selection')
    scalar_result = 195 <= opcode <= 198 or opcode in (201, 214, 215)
    scalar_input = opcode in (202, 216, 217)
    mask = (8 if form == 1 else 5) if scalar_result else (12 if form == 1 else 15)
    require(report['mask'] == mask, 'Wrong double destination mask')
    flags = 106 | (1 << 12) | ((1 << 17) if opcode >= 210 else 0)
    count = 3 if opcode in (200, 211) else (2 if opcode <= 198 or opcode == 210 else 1)
    values = [struct.unpack('<4I', struct.pack('<2d', *pair))
              for pair in ((1.5, -2.25), (0.5, 4.0), (-1.0, 2.0))]
    if scalar_input:
        values[0] = (0x3fc00000, 0xc0100000, 0x40800000, 0x3f000000) if opcode == 202 else (3, 0xfffffffd, 5, 9)
    if opcode == 200:
        values[0] = (1, 0, 0, 1)
    operands = [(0x100002 | (mask << 4), 0)]
    for i in range(count):
        scalar = scalar_input or (opcode == 200 and i == 0)
        if form == 2:
            operands.append((0x4002 if scalar else 0x5002, *values[i]))
        elif form == 1 and not scalar:
            operands.append((0x802084e6, 0xc1, 0, i))
        else:
            operands.append((0x208e46, 0, i))
    rows = [instruction(flags), instruction(89, (0x208e46, 0, count)), instruction(104, (1,))]
    if stage == 5:
        rows.append(instruction(155, (1, 1, 1)))
    rows += [instruction(54, (0x1000f2, 0), (0x4002, 0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00)),
             instruction(opcode, *operands), instruction(62)]
    validity = (11 if scalar_result else 3) if form == 3 else 15
    return rows, validity


def verify(report, native, oracle, reference):
    require(report['schema'] == 1, 'Wrong evidence schema')
    vendor = report['vendor']
    require(vendor in (0, 4318, 32902), 'Wrong requested adapter')
    identity = report['identity']
    require(identity['backend'] == 'dxbc-gpu/21' and identity['warp'] == (vendor == 0), 'Wrong backend identity')
    require(identity['vendor_id'] == (vendor or 5140), 'Adapter substitution')
    require(all(identity[k] == reference[k] for k in ('vendor_id', 'device_id', 'driver_version', 'luid_low', 'luid_high')),
            'Reference and helper adapters differ')
    require(report['native_sha256'] == digest(native), 'Native bytecode changed')
    stage, parsed = shader_rows(native)
    require(stage == report['stage'], 'Native stage changed')
    rows = [row for _, row in parsed]
    raw = report['scope'] == 'double_instruction_operand_semantics'
    require(raw or report['scope'] == 'compiled_double_shader_semantics', 'Wrong evidence scope')
    if raw:
        expected_rows, valid = raw_rows(report)
        require(rows == expected_rows, 'Native operand encoding changed')
        require(report['extended_support'] is (vendor != 32902), 'Changed local extended capability')
    else:
        require(stage == 1 and report['scenario'] in SCENARIOS and report['dataset'] in range(3), 'Wrong compiled selection')
        require(any(row[0] & 2047 in OPCODES for row in rows), 'Compiled shader has no double instructions')
        valid = 15
    require(report['basic_support'] is (vendor != 32902), 'Changed local basic capability')
    if vendor == 32902:
        require(report['outcome'] == 'hardware_unsupported' and oracle is None and report['oracle_sha256'] is None,
                'Unsupported hardware presented as executed')
        if raw:
            require(report['gpu_operations'] == 0 and report['state_unchanged'] is True,
                    'Unsupported instruction modified state')
            require(report['error'] == 'Selected adapter does not support double shader instructions',
                    'Wrong rejection reason')
        return False
    require(report['outcome'] == 'compared' and oracle is not None, 'Missing independent comparison')
    require(report['oracle_sha256'] == digest(oracle), 'Oracle bytecode changed')
    oracle_stage, oracle_rows = shader_rows(oracle)
    require(oracle_stage == 1, 'Wrong oracle stage')
    if raw:
        require(report['expected_valid'] == valid and report['gpu_operations'] == 1 and report['oracle_stage'] == 1,
                'Wrong instruction result metadata')
        require(sum(row == rows[-2] for _, row in oracle_rows) == 1, 'Original instruction absent from independent oracle')
    else:
        require(native == oracle and report['reverse_journal_verified'] is True and report['gpu_operations'] > 0,
                'Compiled shader or reverse journal differs')
    require(report['valid'] == valid, 'Wrong paired result validity')
    actual, expected = report['actual'], report['expected']
    require(len(actual) == len(expected) == 4 and actual == expected, 'Raw result bits differ')
    require(all(type(v) is int and 0 <= v <= 0xffffffff for v in actual), 'Invalid raw register word')
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--test-log', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    log = args.test_log.read_text(encoding='utf-8')
    require('0 failed, 0 skipped, 0 blacklisted' in log and 'FAIL!' not in log, 'Test execution failed or skipped')
    references = {}
    for function, tag, payload in re.findall(r'QINFO\s+: DxbcExecutionTests::(doubleInstructions|compiledDoubles)\(([^)]+)\) (\{[^\n]+\})', log):
        record = json.loads(payload)
        if 'adapter' in record:
            references[('raw' if function == 'doubleInstructions' else 'compiled', tag)] = record
    seen, artifacts, controls = set(), [], {}
    compared = unsupported = 0
    for path in sorted(args.directory.glob('*.json')):
        report = json.loads(path.read_text(encoding='utf-8'))
        raw = report['scope'] == 'double_instruction_operand_semantics'
        key = ('raw', *(report[k] for k in ('vendor', 'stage', 'opcode', 'form'))) if raw else (
            'compiled', *(report[k] for k in ('vendor', 'scenario', 'dataset')))
        require(key not in seen, 'Duplicate double evidence')
        seen.add(key)
        tag = '-'.join(map(str, key[1:] if raw else key[1:-1]))
        reference = references[(key[0], tag)]
        native_path = path.with_suffix('.native.dxbc')
        oracle_path = path.with_suffix('.oracle.dxbc')
        native = native_path.read_bytes()
        oracle = oracle_path.read_bytes() if oracle_path.exists() else None
        success = verify(report, native, oracle, reference)
        compared += success
        unsupported += not success
        for file in (path, native_path, *([oracle_path] if oracle is not None else [])):
            data = file.read_bytes()
            artifacts.append(dict(path=str(file), bytes=len(data), sha256=digest(data)))
        changes = {'backend': lambda r: r['identity'].update(backend='changed'),
                   'adapter': lambda r: r['identity'].update(luid_low=0),
                   'bytecode': lambda r: r.update(native_sha256='changed'),
                   'scope': lambda r: r.update(scope='complete_shader_debugging'),
                   'outcome': lambda r: r.update(outcome='changed')}
        if success:
            changes.update(bits=lambda r: r['actual'].__setitem__(0, r['actual'][0] ^ 1),
                           validity=lambda r: r.update(valid=r['valid'] ^ 1),
                           oracle=lambda r: r.update(oracle_sha256='changed'))
        elif raw:
            changes.update(state=lambda r: r.update(state_unchanged=False))
        for name, mutate in changes.items():
            changed = copy.deepcopy(report)
            mutate(changed)
            try:
                verify(changed, native, oracle, reference)
            except ValueError as error:
                controls[f'{path.stem}-{name}'] = str(error)
            else:
                raise ValueError('Corrupted double evidence accepted: ' + name)
    expected = {('raw', *key) for key in itertools.product((0, 4318, 32902), (0, 1, 5), OPCODES, range(4))}
    expected |= {('compiled', *key) for key in itertools.product((0, 4318, 32902), SCENARIOS, range(3))}
    require(seen == expected, 'Incomplete double evidence matrix')
    result = dict(schema=1, scope='local_double_instruction_and_compiled_vs_comparisons',
                  cases=len(seen), compared=compared, hardware_unsupported=unsupported,
                  negative_controls=controls, artifacts=artifacts)
    with args.out.open('x', encoding='utf-8') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(json.dumps(dict(cases=len(seen), compared=compared, hardware_unsupported=unsupported, negative_controls=len(controls))))


if __name__ == '__main__':
    main()
