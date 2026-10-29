"""Check resource-route disassembly, original-code observations and guarded writes."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path
import struct

from probe_original_host_resource_route import SHA, CODE
from probe_original_resource_reconstruction import DESCRIPTORS


def require(value, message):
    if not value:
        raise ValueError(message)


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_probe(report):
    require(report['schema'] == 1 and report['completed'] and not report['engine_recovered'] and
            not report['gpu_executed'] and report['player_sha256'] == SHA and
            report['scope'] == 'guarded_original_resource_request_routes' and report['code_prefixes'] == CODE,
            'Probe scope, completion or binary identity')
    rows = report['cases']
    require(len(rows) == 36 and {(r['pattern'], r['identifier'], r['mode'], r['alias']) for r in rows} ==
            set(itertools.product((0xa5, 0x5a), (1, 0x80000000, 0xffffffff),
                                  ('missing', 'null', 'unknown'), (False, True))), 'Route matrix')
    for row in rows:
        addresses = row['addresses']
        index = (1, 0x80000000, 0xffffffff).index(row['identifier'])
        require(row['selected_node'] == index and row['result'] == 0, 'Route selection/result')
        require(row['lookups'] == dict(found=1, absent=0, file=addresses[f'file_{index}'] + 32,
                                      api=addresses[f'api_{index}'] + 40, null_api=None), 'Lookup ABI')
        require(row['callbacks'] == ([] if row['mode'] == 'missing' else
                [dict(this=addresses[f'api_{index}'] + 40, selector=0)]), 'Callback ABI')
        expected = {name: bytearray.fromhex(value) for name, value in row['before'].items()}
        require(set(expected) == set(addresses) == set(row['after']), 'Guarded block inventory')
        for data in expected.values():
            require(data[:64] == bytes([row['pattern']]) * 64 and
                    data[-64:] == bytes([row['pattern']]) * 64, 'Initial guards')
        host = bytearray([row['pattern']]) * (0x3d0 + 128)
        struct.pack_into('<Q', host, 64 + 0x360, addresses['file_head'])
        struct.pack_into('<Q', host, 64 + 0xc0, addresses['api_head'])
        require(host == expected['host'], 'Host layout seed')
        request = bytearray([row['pattern']]) * (0xa8 + 128)
        struct.pack_into('<I', request, 64 + 8, 2 if row['mode'] == 'missing' else row['identifier'])
        struct.pack_into('<Q', request, 64 + 0x58, addresses['host'])
        struct.pack_into('<3Q', request, 64 + 0x78, 11, 12, 13)
        struct.pack_into('<3Q', request, 64 + 0x90, 21, 22, 23)
        require(request == expected['request'], 'Request layout seed')
        if row['mode'] != 'missing':
            struct.pack_into('<Q', expected['request'], 64 + 0x60,
                             0 if row['mode'] == 'null' else addresses['marker'])
        if row['mode'] == 'unknown' and not row['alias']:
            for name, offset in (('first', 0x78), ('second', 0x90)):
                expected[name][64:88], expected['request'][64 + offset:88 + offset] = (
                    expected['request'][64 + offset:88 + offset], expected[name][64:88])
        require({name: bytes(value).hex() for name, value in expected.items()} == row['after'],
                'Unexpected guarded write')


def validate_reconstruction(report):
    require(report['completed'] and not report['engine_recovered'] and not report['gpu_executed'] and
            report['player_sha256'] == SHA and report['schema'] == 1, 'Reconstruction scope')
    rows = report['cases']
    require(len(rows) == 8 and {(r['kind'], r['pattern']) for r in rows} ==
            set(itertools.product(DESCRIPTORS, (0xa5, 0x5a))), 'Reconstruction matrix')
    tables = {0x84: 0x1852f0, 0x85: 0x185328, 0x86: 0x185360, 0x88: 0x185250}
    for row in rows:
        kind, addresses = row['kind'], row['addresses']
        constructor, descriptor = DESCRIPTORS[kind]
        require(row['constructor'] == constructor and row['table_rva'] == tables[kind] and
                row['descriptor'] == descriptor and row['result'] == 1, 'Original resource construction')
        expected = {name: bytearray.fromhex(value) for name, value in row['before'].items()}
        for value in expected.values():
            require(value[:64] == value[-64:] == bytes([row['pattern']]) * 64, 'Reconstruction guards')
        host = bytearray([row['pattern']]) * (0x3d0 + 128)
        struct.pack_into('<Q', host, 64 + 0x360, addresses['file_head'])
        struct.pack_into('<Q', host, 64 + 0xc0, addresses['api_head'])
        require(expected['host'] == host, 'Reconstruction host seed')
        require(struct.unpack_from('<I', expected['player'], 64 + 0xc)[0] == 999 and
                struct.unpack_from('<Q', expected['request'], 64 + 0x58)[0] == addresses['host'] and
                expected['resource'][64 + 0x1c:64 + 0x1c + len(descriptor) * 4] ==
                struct.pack('<' + 'I' * len(descriptor), *descriptor), 'Reconstruction input identity')
        struct.pack_into('<Q', expected['request'], 64 + 0x60, addresses['resource'])
        count = 1 if kind == 0x88 else 2
        require(row['id_callback'] == dict(table_rva=0x184c08, invoke_rva=0xb1510, target_rva=0xab6a0,
                                          seed=999, final=999 + count) and
                row['resource_calls'] == [dict(this=addresses['api_0'] + 40, selector=0)],
                'Reconstruction callbacks')
        struct.pack_into('<I', expected['player'], 64 + 0xc, 999 + count)
        for name, stride in (('first', 15), ('second', 16)):
            after = bytes.fromhex(row['after'][name])
            begin, end, capacity = struct.unpack_from('<3Q', after, 64)
            require(0 < begin <= end <= capacity and end - begin == count * stride and
                    capacity - begin <= 65536, 'Reconstruction vector bounds')
            expected[name][64:88] = after[64:88]
        require({name: bytes(value).hex() for name, value in expected.items()} == row['after'],
                'Unexpected reconstruction write')
        payloads = [bytes(16) + struct.pack('<' + 'I' * len(descriptor), *descriptor) +
                    (b'' if kind == 0x88 else bytes(8))]
        if kind != 0x88:
            # These are observed GPA serialized fields, not a claim that the
            # resulting D3D11 SRV has been created or bound successfully.
            payloads.append(bytes(16) + struct.pack('<8I', 1000, 0, 28 if kind == 0x85 else 0,
                                                    {0x84: 2, 0x85: 4, 0x86: 8}[kind], 0, 1, 0, 0))
        records = b''.join(struct.pack('<IIHIB', 1000 + i, 2, kind if i == 0 else 0x8c, len(p), 0)
                           for i, p in enumerate(payloads))
        require(row['payloads'] == [p.hex() for p in payloads] and row['records'] == records.hex(),
                'Serialized resource output')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    root = args.directory
    static = root / 'verified-complete'
    exports = {}
    for name, count in (('request', 2), ('helpers', 8), ('writers', 9), ('constructors', 5),
                        ('callbacks', 4), ('types', 9)):
        text = (static / (name + '.txt')).read_text(encoding='utf-8')
        require(text.splitlines()[0] == f'PROGRAM dx11_player.dll SHA256 {SHA} BASE 180000000' and
                text.splitlines()[-1] == f'COMPLETE {count}' and 'NO FUNCTION ' not in text,
                'Incomplete Ghidra export: ' + name)
        exports[name] = text
    for item in read(static / 'evidence-sha256.json'):
        require(Path(item['name']).name == item['name'] and
                digest(static / item['name']) == item['sha256'], 'Changed Ghidra evidence')
    edges = {
        'request': ['0xabade MOV RCX,qword ptr [RCX + 0x58]', '0xabae5 ADD RCX,0x1d8',
                    '0xabaf4 CALL 0x1800865a0', '0xabb0e CALL 0x180085f40',
                    '0xabb19 ADD RCX,0x48', '0xabb22 CALL 0x18009b350',
                    '0xabb2c MOV R8,qword ptr [RCX + 0x18]', '0xabb33 CALL R8',
                    '0xabb39 MOV qword ptr [RBX + 0x60],RCX', '0xabc2b LEA RDX,[RBX + 0x78]',
                    '0xabc60 ADD RBX,0x90'],
        'helpers': ['0x865b9 MOV R8,qword ptr [RCX + 0x188]',
                    '0x85f97 LEA RAX,[RCX + 0x20]', '0x9b371 MOV R8,qword ptr [RCX + 0x78]',
                    '0x9b3a9 LEA RAX,[RCX + 0x28]', '0xaa808 MOV dword ptr [RBX + 0x70],EAX',
                    '0xaafd7 LEA R9,[0x180200458]', '0xab175 LEA R9,[0x180200538]',
                    '0xab359 LEA R9,[0x180200618]'],
        'writers': ['0xb264a MOV qword ptr [RCX],RAX', '0xa3c5d CALL 0x1800a6230',
                    '0xa3cd6 CALL 0x1800a5ff0', '0xa3c56 LEA RCX,[RSI + 0x90]',
                    '0xa3cd2 LEA RCX,[RSI + 0x78]'],
        'constructors': ['0xb26fa MOV qword ptr [RCX],RAX', '0xb272a MOV qword ptr [RCX],RAX',
                         '0xb275a MOV qword ptr [RCX],RAX'],
        'callbacks': ['0xb1510 MOV RAX,qword ptr [RCX + 0x8]',
                      '0xb1514 MOV RCX,qword ptr [RCX + 0x10]', '0xb1518 JMP RAX',
                      '0xab6b0 INC EAX', '0xab6b2 MOV dword ptr [RCX + 0xc],EAX',
                      '0xab6bb CALL 0x180087bb0', '0xab6c0 BTS EAX,0x1f',
                      '0x8653e MOV EAX,dword ptr [RAX + 0x1c]'],
    }
    for name, instructions in edges.items():
        require(all('ASM RVA ' + instruction in exports[name] for instruction in instructions),
                'Missing decoded edge: ' + name)
    require(all('Playback::ID3D11' + kind + 'Rsc::' in exports['types']
                for kind in ('Texture1D', 'Texture2D', 'Texture3D', 'SamplerState')), 'Resource RTTI')
    run, report = read(root / 'probe-final/run.json'), read(root / 'probe-final/routes.json')
    require(run['completed'] and run['exit_code'] == 0 and not run['timed_out'], 'Original child failed')
    validate_probe(report)
    reconstruction = read(root / 'reconstruction-original/reconstruction.json')
    reconstruction_run = read(root / 'reconstruction-original/run.json')
    require(reconstruction_run['completed'] and reconstruction_run['exit_code'] == 0 and
            not reconstruction_run['timed_out'], 'Reconstruction child failed')
    validate_reconstruction(reconstruction)
    require(report['source_sha256'] == digest(Path('tools/probe_original_host_resource_route.py')) and
            reconstruction['source_sha256'] == digest(Path('tools/probe_original_resource_reconstruction.py')),
            'Changed original probe source')
    rejected = 0
    for index in (0, 2, 4):
        for field in ('host', 'request', 'file_1', 'first'):
            changed = copy.deepcopy(report)
            raw = bytearray.fromhex(changed['cases'][index]['after'][field])
            raw[64 + 0x3c0 if field == 'host' else 0] ^= 1
            changed['cases'][index]['after'][field] = raw.hex()
            try:
                validate_probe(changed)
            except ValueError:
                rejected += 1
            else:
                raise ValueError('Corrupted memory accepted')
    for index in range(0, 8, 2):
        for field in ('records', 'payloads'):
            changed = copy.deepcopy(reconstruction)
            if field == 'records':
                changed['cases'][index]['records'] = '00' + changed['cases'][index]['records'][2:]
            else:
                changed['cases'][index]['payloads'][0] = 'ff' + changed['cases'][index]['payloads'][0][2:]
            try:
                validate_reconstruction(changed)
            except ValueError:
                rejected += 1
            else:
                raise ValueError('Corrupted serialized output accepted')
    sources = [Path('tools/probe_original_host_resource_route.py'), Path(__file__),
               Path('tools/probe_original_resource_reconstruction.py'),
               Path('tools/ghidra/Invoke-HostResourceResearch.ps1')]
    files = sorted(p for p in root.rglob('*') if p.is_file()) + sources
    result = dict(schema=1, passed=True, engine_recovered=False, full_release_acceptance=False,
                  cases=36, reconstruction_cases=8, negative_checks=rejected, elapsed_ms=run['elapsed_ms'],
                  reconstruction_elapsed_ms=reconstruction_run['elapsed_ms'],
                  edges=edges, player_sha256=SHA,
                  evidence=[dict(path=p.as_posix(), bytes=p.stat().st_size, sha256=digest(p)) for p in files])
    args.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(passed=True, cases=36, reconstruction_cases=8, negative_checks=rejected, evidence=len(files))))


if __name__ == '__main__':
    main()
