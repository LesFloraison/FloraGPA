"""Development-only decoding of the observed legacy Intel RC write descriptors.

Consumes original GPA ISA text, not captures. This deliberately accepts only
the SIMD8/M0, FP32 write forms investigated here; it is not an ISA disassembler.
Field provenance: Intel IGC MessageDecoderOther.cpp::tryDecodeRC (pre-Xe2).
"""
import argparse
import json
from pathlib import Path
import re


WRITE = re.compile(
    r'^\s*(?P<predicate>\([^)]*\))?\s*sendc?\.rc\s+\(8\|M0\)\s+'
    r'null\s+(?P<src0>r\d+)\s+(?P<src1>null|r\d+)\s+'
    r'(?P<extended>0x[0-9a-fA-F]+)\s+(?P<descriptor>0x[0-9a-fA-F]+)'
    r'\s*(?P<options>\{[^}]*\})?\s*$')


def inspect(text):
    messages = []
    for number, line in enumerate(text.splitlines(), 1):
        code = line.split('//', 1)[0]
        if not re.search(r'\bsendc?\.rc\b', code):
            continue
        match = WRITE.fullmatch(code)
        if match is None:
            raise ValueError(f'Unsupported RC instruction form at line {number}')
        desc, extended = (int(match[name], 16) for name in ('descriptor', 'extended'))
        if max(desc, extended) > 0xffffffff:
            raise ValueError(f'Descriptor exceeds uint32 at line {number}')
        if desc >> 14 & 15 != 12 or desc >> 30 & 1:
            raise ValueError(f'Expected legacy FP32 render write at line {number}')
        subtype = desc >> 8 & 7
        if subtype not in (2, 4):
            raise ValueError(f'Unverified render write subtype at line {number}')
        messages.append(dict(line=number,descriptor=f'0x{desc:08x}',
                             extended_descriptor=f'0x{extended:x}',
                             binding_table_index=desc & 255,
                             subtype_bits=subtype,
                             subtype='lo8ds' if subtype == 2 else 'simd8',
                             fp32=True,last_render_target=bool(desc >> 12 & 1),
                             predicate=match['predicate'],
                             end_of_thread=bool(re.search(r'\bEOT\b', match['options'] or ''))))
    if not messages:
        raise ValueError('No supported render write found')
    return dict(schema=1,acceptance_established=False,platform_scope='legacy RC, pre-Xe2',
                scope='Descriptor fields in exported ISA only; no execution or shader-equivalence proof.',
                render_writes=messages)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('isa', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = inspect(args.isa.read_text(encoding='utf-8'))
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')


if __name__ == '__main__':
    main()
