"""Independently check original caller evidence, never shader-debug parity."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct

SHA = '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b'


def require(condition, label):
    if not condition:
        raise ValueError(label)


def check(report, row):
    require(report['completed'] and report['closed'] and report['player_unchanged'] and
            report['capture_unchanged'], 'Lifecycle')
    require(report['player_sha256'] == SHA and not report['engine_recovered'] and
            not report['shader_debug_parity'], 'Identity or recovery claim')
    require(report['scope'] == 'synthetic_engine_call_contract_not_original_engine', 'Scope')
    require(not report['errors'] and not report['observer_errors'], 'Probe errors')
    require(report['replay_adapter'] == report['replay_adapter_after'] and
            report['replay_adapter']['vendor'] == row['vendor'], 'Adapter')
    require(all(s['engine'] == '0x0' for s in report['snapshots']), 'Unrestored host')
    require(len(report['playbacks']) == 2 and
            all(p['status'] == 0 and p['error'] == '0x0' for p in report['playbacks']), 'Replay')
    contract = report['synthetic_contract']
    require(not contract['original_engine'] and not contract['callback_errors'] and
            contract['restored'] and contract['installed_field_preserved'] and
            contract['closed_singleton'] == 0, 'Receiver lifecycle')
    require(contract['storage_before'] == contract['storage_after'] == contract['storage_after_close'],
            'Receiver guard')
    storage = bytes.fromhex(contract['storage_before'])
    require(len(storage) == 0x88 and storage[:0x40] == bytes([0xa5] * 0x40) and
            storage[0x48:] == bytes([0xa5] * 0x40), 'Receiver allocation')
    request = contract['request']
    selection = [((row['seed'] + i * 0x10203) & 0xffffffff) if row['seed'] else 0 for i in range(8)]
    selection[1] = row['event']
    require(request['selection'] == selection and request['version'] == 0 and
            request['manager'] == request['owner'] + 0xf8, 'Request fields')
    require(len(contract['calls']) == len(contract['results']) == len(contract['destructors']) == 1,
            'Callback counts')
    call, result, destructor = contract['calls'][0], contract['results'][0], contract['destructors'][0]
    require(call['receiver'] == contract['receiver'] and call['request'] == request['address'] and
            call['selection_address'] == request['address'] + 0x20 and
            call['selection'] == selection and call['version'] == request['version'], 'Call arguments')
    require(call['status_before'] == call['canonical_status'] == 0 and
            call['output'] == call['status'] + 8, 'Status/output arguments')
    expected = bytearray(0xa0)
    struct.pack_into('<Q', expected, 0x60, 15)
    struct.pack_into('<Q', expected, 0x98, 15)
    require(call['output_before'] == result['bytes'] == expected.hex() and
            result['output'] == call['output'] and result['userdata'] is None, 'Empty result callback')
    require(destructor == dict(receiver=contract['receiver'], flags=1,
                               installed_engine=contract['receiver']), 'Destructor dispatch')
    require(len(report['requests']) == 2 and all(r['status'] == 0 and r['error'] == '0x0' and
            r['observer_restored'] for r in report['requests']), 'Request completion')
    require(not report['requests'][0]['execution'] and not report['requests'][0]['result_callbacks'],
            'Null-engine control')
    executions = report['requests'][1]['execution']
    require(len(executions) == 1 and executions[0] == dict(request_vtable_rva='0x183198',
            host_matches_global=True, engine_before='0x0', version=0, selection=selection,
            returned=1, engine_after='0x0'), 'Observed execution')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--matrix', default='final', help='Matrix directory below the evidence root')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    static = (args.root / 'verified/functions.txt').read_text(encoding='utf-8')
    require(static.startswith(f'PROGRAM dx11_player.dll SHA256 {SHA} BASE 180000000\n') and
            static.endswith('COMPLETE 5\n'), 'Static export identity')
    for line in ['ASM RVA 0x902e4 MOV RBX,qword ptr [RDX + 0x98]',
                 'ASM RVA 0x902eb MOV RAX,qword ptr [RBX + 0x3c0]',
                 'ASM RVA 0x905de LEA R8,[RSI + 0x20]',
                 'ASM RVA 0x905ea MOV qword ptr [RSP + 0x20],RDX',
                 'ASM RVA 0x905ef MOV R9D,dword ptr [RSI + 0x10]',
                 'ASM RVA 0x905fb CALL qword ptr [RAX]',
                 'ASM RVA 0x8a038 CALL qword ptr [RAX + 0x8]',
                 'ASM RVA 0x8a03b MOV qword ptr [RBX + 0x3c0],RDI']:
        require(line in static.splitlines(), f'Missing instruction {line}')
    runs = json.loads((args.root / args.matrix / 'runs.json').read_text(encoding='utf-8'))
    require(runs['completed'] and len(runs['runs']) == 12, 'Incomplete matrix')
    keys, rejected = set(), 0
    for row in runs['runs']:
        require(row['completed'] and row['exit_code'] == 0, 'Process failure')
        keys.add((row['name'], row['vendor'], row['seed']))
        report = json.loads(Path(row['report']).read_text(encoding='utf-8'))
        for source, actual in [('probe_original_debugger_host.py', report['source_sha256']),
                               ('probe_original_engine_contract.py', report['synthetic_contract']['source_sha256'])]:
            require(hashlib.sha256(Path(__file__).with_name(source).read_bytes()).hexdigest() == actual,
                    'Stale probe source')
        check(report, row)
        # The validator must reject corrupted evidence, not merely parse JSON.
        mutations = [('engine_recovered', True), ('closed', False), ('player_sha256', '0' * 64)]
        for field, value in mutations:
            altered = copy.deepcopy(report)
            altered[field] = value
            try:
                check(altered, row)
            except ValueError:
                rejected += 1
            else:
                raise ValueError('Corruption accepted')
        for collection, field in [('calls', 'selection_address'), ('calls', 'version'),
                                  ('calls', 'receiver'), ('destructors', 'flags'), ('results', 'output')]:
            altered = copy.deepcopy(report)
            altered['synthetic_contract'][collection][0][field] += 1
            try:
                check(altered, row)
            except ValueError:
                rejected += 1
            else:
                raise ValueError('Callback corruption accepted')
    require(keys == {(n, v, s) for n in ('gf2', 'bf1', 'helldivers')
                     for v in (4318, 32902) for s in (0, 0x13579bdf)}, 'Matrix coverage')
    control = json.loads((args.root / 'default-control/dispatch.json').read_text(encoding='utf-8'))
    require(control['completed'] and control['closed'] and not control['errors'] and
            not control['observer_errors'] and 'synthetic_contract' not in control and
            control['player_unchanged'] and control['capture_unchanged'], 'Default-mode control')
    require(control['replay_adapter'] == control['replay_adapter_after'] and
            control['replay_adapter']['vendor'] == 4318, 'Default-mode adapter')
    require(all(s['engine'] == '0x0' for s in control['snapshots']) and
            len(control['requests']) == 2 and all(r['status'] == 0 and r['error'] == '0x0' and
            r['observer_restored'] and not r['result_callbacks'] for r in control['requests']),
            'Default-mode dispatch')
    ledger = dict(schema=1, status='caller_contract_verified', cases=len(keys), corruptions_rejected=rejected,
                  default_mode_controls=1, original_engine_recovered=False, shader_execution_verified=False)
    args.out.write_text(json.dumps(ledger, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(ledger))


if __name__ == '__main__':
    main()
