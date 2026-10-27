"""Validate bounded debugger-alias scans and original state-block layout evidence."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct


SHA = '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b'
EXPORTS = {'host-plus8': 16, 'host-plus48': 6, 'host-plusf8': 11,
           'host-plus8-addresses': 10, 'host-plus48-addresses': 12, 'host-plusf8-addresses': 3,
           'nonstack-stores': 4, 'api-aliases': 6, 'state-tables': 2, 'state-symbols': 54,
           'state-owner': 2, 'host-owner': 4}


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_layout(report):
    require(report['schema'] == 1 and report['scope'] == 'original_state_block_synthetic_layout' and
            report['completed'] is True and report['engine_recovered'] is False and report['player_sha256'] == SHA,
            'Wrong layout report identity or scope')
    base = report['module_base']
    require(type(base) is int and base > 0 and base % 0x10000 == 0, 'Invalid player module base')
    require(report['rtti'] == dict(name='.?AVStateBlockD3D11@Playback@@', locator_rva=0x1c3b28,
            type_rva=0x1ffaa8, hierarchy_rva=0x1c3b50, vtable_rva=0x179318), 'Wrong runtime RTTI identity')
    rows = report['cases']
    require(len(rows) == 6 and {(r['pattern'], r['marker']) for r in rows} ==
            {(p, m) for p in (0xa5, 0x5a) for m in (0, 4, 0x11223344)}, 'Incomplete layout matrix')
    padding = {0xc, 0x3c, 0xbc, 0xf4, 0x12c, 0x164, 0x19c, 0x1d4, 0x20c,
               0x244, 0x2d4, 0x334, 0x374, 0x3ac, 0x3ec, 0x42c}
    for row in rows:
        require(row['completed'] is True and row['returned_input'] is True, 'Incomplete original lifecycle')
        before = bytes.fromhex(row['before'])
        require(before == bytes([row['pattern']]) * 0x510, 'Wrong guarded storage seed')
        for key, vtable in (('after_constructor', 0x179318), ('after_destructor', 0x1792c8)):
            actual = bytes.fromhex(row[key])
            require(len(actual) == len(before) and actual[:0x40] == before[:0x40] and
                    actual[-0x40:] == before[-0x40:], 'Guard storage changed')
            body = actual[0x40:-0x40]
            require(struct.unpack_from('<Q', body)[0] == base + vtable and
                    struct.unpack_from('<I', body, 8)[0] == row['marker'], 'Wrong state-block header')
            for offset in range(0xc, 0x490, 4):
                expected = bytes([row['pattern']]) * 4 if offset in padding else bytes(4)
                require(body[offset:offset + 4] == expected, f'Unexpected state-block write at {offset:#x}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    static = args.directory / 'verified-final'
    loaded = {}
    for name, count in EXPORTS.items():
        text = (static / (name + '.txt')).read_text(encoding='utf-8')
        lines = text.splitlines()
        require(lines[0] == f'PROGRAM dx11_player.dll SHA256 {SHA} BASE 180000000' and
                lines[-1] == f'COMPLETE {count}' and 'NO FUNCTION ' not in text, 'Incomplete Ghidra export: ' + name)
        log = (static / (name + '.log')).read_text(encoding='utf-8-sig')
        require('ERROR REPORT SCRIPT ERROR' not in log, 'Failed Ghidra script: ' + name)
        loaded[name] = text
    for manifest in json.loads((static / 'evidence-sha256.json').read_text(encoding='utf-8-sig')):
        require(digest(static / manifest['name']) == manifest['sha256'], 'Static export hash changed')
    binary = json.loads((static / 'binary.json').read_text(encoding='utf-8-sig'))
    require(binary['sha256'] == SHA and digest(Path(binary['path'])) == SHA, 'Installed player identity changed')
    for line in ('ASM RVA 0xb2873 LEA RAX,[0x180179318]',
                 'ASM RVA 0xb2aad MOV qword ptr [RCX + 0x2c8],RAX',
                 'ASM RVA 0xb2b44 MOV qword ptr [RCX + 0x378],RAX',
                 'ASM RVA 0xb2b7b MOV qword ptr [RCX + 0x3b8],RAX',
                 'ASM RVA 0x6d3b8 LEA RCX,[RBX + 0x38ac]',
                 'ASM RVA 0x114bdc LEA RBP,[RSP + -0x3f0]'):
        require(line in loaded['nonstack-stores'], 'Missing candidate register provenance')
    require('SYMBOL Playback::StateBlockD3D11::vftable' in loaded['state-tables'] and
            'ASM RVA 0x61667 CALL 0x1800b2870' in loaded['state-owner'] and
            'ASM RVA 0x3a79f CALL 0x1800b3170' in loaded['state-owner'], 'Missing state-block ownership evidence')
    report = json.loads((args.directory / 'layout' / 'layout.json').read_text(encoding='utf-8'))
    validate_layout(report)
    require(report['source_sha256'] == digest(Path('tools/probe_original_state_block_layout.py')),
            'Original probe source changed')
    run = json.loads((args.directory / 'layout' / 'run.json').read_text(encoding='utf-8'))
    require(run['exit_code'] == 0 and run['timed_out'] is False and run['timeout_seconds'] == 30,
            'Original probe did not complete within its deadline')
    controls = 0
    for index in range(6):
        for field, offset in (('after_constructor', 0), ('after_constructor', 0x40),
                              ('after_constructor', 0x48), ('after_constructor', 0x40 + 0x3c0),
                              ('after_destructor', 0x40 + 0x378)):
            bad = copy.deepcopy(report)
            raw = bytearray.fromhex(bad['cases'][index][field])
            raw[offset] ^= 1
            bad['cases'][index][field] = raw.hex()
            try:
                validate_layout(bad)
            except ValueError:
                controls += 1
            else:
                raise ValueError('Accepted corrupted original layout evidence')
    paths = sorted(p for p in args.directory.rglob('*') if p.is_file())
    paths += [Path(p) for p in ('tools/probe_original_state_block_layout.py',
                               'tools/validate_host_alias_research.py', 'tools/ghidra/Invoke-HostAliasResearch.ps1',
                               'tools/ghidra/ExportFieldStores.java', 'tools/ghidra/ExportFunctionEvidence.java',
                               'tools/ghidra/ExportAddressEvidence.java', 'tools/ghidra/ExportSymbolEvidence.java')]
    output = dict(schema=1, scope='debugger_alias_candidate_evidence', completed=True,
                  engine_recovered=False, whole_program_alias_closure=False, synthetic_lifecycles=6,
                  candidate_stores=33, candidate_addresses=25, verified_exports=12, negative_controls=controls,
                  files=[dict(path=p.as_posix(), bytes=p.stat().st_size, sha256=digest(p)) for p in paths])
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(output, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: v for k, v in output.items() if k != 'files'}))


if __name__ == '__main__':
    main()
