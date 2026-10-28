"""Validate bounded original host accessor observations and Ghidra call edges."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path


SHA = '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b'
EXPORTS = {'callers': 4, 'alternate': 3, 'global': 1, 'accessors': 6,
           'consumers': 3, 'request': 3, 'types': 6}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_probe(report):
    require(report['schema'] == 1 and report['scope'] == 'original_host_synthetic_accessors' and
            report['player_sha256'] == SHA and report['completed'] is True and
            report['engine_recovered'] is False and report['singleton_restored'] is True,
            'Incomplete or incorrectly scoped original probe')
    require(report['null'] == dict(raw=None, active=None, flag=0), 'Wrong null results')
    require(report['code'] == {'0x8ab90': '8891f8000000c3',
            '0x89420': '488b0519f11700c3',
            '0x87bd0': '488b05690918004885c07405803800750233c0c3',
            '0x87bf0': '488b05490918004885c0740b80b898010000000f95c0c3c3'},
            'Changed original accessor machine code')
    rows = report['cases']
    require(len(rows) == 54 and
            {(r['pattern'], r['enabled'], r['stored_flag'], r['value']) for r in rows} ==
            set(itertools.product((0xa5, 0x5a), (0, 1, 0xff), (0, 1, 0x80), (0, 1, 0xff))),
            'Incomplete accessor case matrix')
    for row in rows:
        host = row['host']
        require(type(host) is int and 0 < host < 2 ** 64, 'Invalid synthetic address')
        before = bytearray([row['pattern']] * 0x450)
        before[0x40] = row['enabled']
        before[0x1d8] = row['stored_flag']
        require(bytes.fromhex(row['before']) == bytes(before), 'Incorrect guarded seed')
        before[0x138] = row['value']
        require(bytes.fromhex(row['after']) == bytes(before), 'Unexpected write or changed guard')
        require(row['returned'] == dict(raw=host, active=host if row['enabled'] else None,
                                       flag=int(row['stored_flag'] != 0)), 'Incorrect accessor result')


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
                lines[-1] == f'COMPLETE {count}' and 'NO FUNCTION ' not in text,
                'Incomplete Ghidra export: ' + name)
        require('ERROR REPORT SCRIPT ERROR' not in
                (static / (name + '.log')).read_text(encoding='utf-8-sig'), 'Ghidra script failed')
        loaded[name] = text
    for record in json.loads((static / 'evidence-sha256.json').read_text(encoding='utf-8-sig')):
        require(Path(record['name']).name == record['name'] and
                digest(static / record['name']) == record['sha256'], 'Changed static evidence')
    edges = {
        'callers': ['0x87ce3 CALL 0x180089c70', '0x87ce9 MOV qword ptr [0x180208540],RAX',
                    '0x898e4 CALL 0x180089c70', '0x898ea MOV qword ptr [0x180208540],RAX',
                    '0x898f8 CALL 0x18008ab90'],
        'alternate': ['0x8ab90 MOV byte ptr [RCX + 0xf8],DL', '0x8ab96 RET',
                      '0xa7cea CALL 0x180089880'],
        'accessors': ['0x89420 MOV RAX,qword ptr [0x180208540]',
                      '0x87bdc CMP byte ptr [RAX],0x0',
                      '0x87bfc CMP byte ptr [RAX + 0x198],0x0'],
        'consumers': ['0x157cc CALL 0x180089420', '0x157d6 MOV RAX,qword ptr [RAX + 0x3b8]',
                      '0x157e2 CALL RAX', '0x5c476 CALL 0x180089420',
                      '0x5c485 JMP 0x18008aab0', '0xaaa55 CALL 0x180087bd0',
                      '0xaab72 MOV qword ptr [RSP + 0x118],RDI', '0xaabd3 CALL 0x1800abac0'],
        'request': ['0x8aaba MOV RCX,qword ptr [RCX + 0x1c8]',
                    '0xabade MOV RCX,qword ptr [RCX + 0x58]', '0xabae5 ADD RCX,0x1d8',
                    '0xabaf4 CALL 0x1800865a0', '0xabb0e CALL 0x180085f40',
                    '0xabb19 ADD RCX,0x48', '0xabb22 CALL 0x18009b350',
                    '0xabb33 CALL R8', '0xabb39 MOV qword ptr [RBX + 0x60],RCX']}
    for name, instructions in edges.items():
        for instruction in instructions:
            require('ASM RVA ' + instruction in loaded[name], 'Missing call edge: ' + instruction)
    require(loaded['global'].count(' WRITE FUNCTION ') == 4, 'Singleton store set changed')
    for rva in ('0x87055', '0x870d0', '0x87ce9', '0x898ea'):
        require('XREF RVA ' + rva + ' WRITE FUNCTION ' in loaded['global'], 'Missing singleton write')
    for symbol in ('Playback::IQueryManager::RTTI_Type_Descriptor',
                   'Playback::GMQueryManager::RTTI_Type_Descriptor',
                   'Playback::IAPIManager::Resource::RTTI_Type_Descriptor',
                   'Playback::ID3D11SamplerStateRsc::RTTI_Type_Descriptor',
                   'PTR_SetThreadExecutionState_180174140', 'PTR_SystemParametersInfoW_180174ab8'):
        require('SYMBOL ' + symbol in loaded['types'], 'Missing type or import evidence')
    binary = json.loads((static / 'binary.json').read_text(encoding='utf-8-sig'))
    require(binary['sha256'] == SHA and digest(Path(binary['path'])) == SHA, 'Installed image changed')
    probe = json.loads((args.directory / 'probe/accessors.json').read_text(encoding='utf-8'))
    validate_probe(probe)
    require(probe['source_sha256'] == digest(Path('tools/probe_original_host_accessors.py')),
            'Probe source changed')
    run = json.loads((args.directory / 'probe/run.json').read_text(encoding='utf-8'))
    require(run['completed'] is True and run['exit_code'] == 0 and run['timed_out'] is False and
            run['timeout_seconds'] == 30, 'Probe deadline or exit failure')
    rejected = 0
    for index in (0, 26, 53):
        for offset in (0, 0x138, 0x400, 0x44f):
            bad = copy.deepcopy(probe)
            raw = bytearray.fromhex(bad['cases'][index]['after'])
            raw[offset] ^= 1
            bad['cases'][index]['after'] = raw.hex()
            try:
                validate_probe(bad)
            except ValueError:
                rejected += 1
            else:
                raise ValueError('Accepted corrupted memory evidence')
    files = sorted(p for p in args.directory.rglob('*') if p.is_file())
    files += [Path(p) for p in ('tools/probe_original_host_accessors.py',
        'tools/validate_host_accessor_research.py', 'tools/ghidra/Invoke-HostAccessorResearch.ps1',
        'tools/ghidra/ExportFunctionEvidence.java', 'tools/ghidra/ExportAddressEvidence.java')]
    result = dict(schema=1, scope='original_host_accessor_call_edges', completed=True,
                  engine_recovered=False, whole_program_alias_closure=False, cases=54,
                  verified_exports=len(EXPORTS), rejected_memory_corruptions=rejected,
                  files=[dict(path=p.as_posix(), bytes=p.stat().st_size, sha256=digest(p)) for p in files])
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'files'}))


if __name__ == '__main__':
    main()
