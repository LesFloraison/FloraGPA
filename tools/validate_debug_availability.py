"""Validate pinned original presenter metadata, guarded reads and static evidence."""
import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path

PINNED = '35a03995d50f390ed74722385583e7bb011f84e563a579c88086d8039ad1df80'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def identity(path):
    return dict(path=path.resolve().as_posix(), bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def validate(report):
    require(report['schema'] == 1 and report['scope'] == 'original_shader_debug_availability_property'
            and report['sha256'] == PINNED and report['passed'] is True, 'Pinned report identity')
    require(all(report[key] is False for key in ('engine_recovered', 'live_application',
                                               'frame_opened', 'shader_executed')), 'Bounded research scope')
    metadata = report['metadata']
    for key, value in dict(class_name='ShadersPresenter', metaobject_rva='0x774f00',
                           strings_rva='0x7754c0', data_rva='0x775d50', dispatcher_rva='0xb66d0',
                           getter_rva='0xd22e0', getter_bytes='32c0c3').items():
        require(metadata[key] == value, 'Presenter metadata identity')
    properties, methods = metadata['properties'], metadata['methods']
    require([p['index'] for p in properties] == list(range(22)) and
            [m['index'] for m in methods] == list(range(25)), 'Complete metadata indexes')
    require(properties[21]['name'] == 'isShaderDebuggerAvailable' and properties[21]['type'] == 1
            and properties[21]['notify'] == 12 and methods[23]['name'] == 'debugShader'
            and methods[23]['argc'] == 0, 'Named property and dispatch selection')
    seen = set()
    for row in report['cases']:
        key = row['seed'], row['object_argument']
        require(key not in seen and row['seed'] in (0, 85, 255), 'Unique guarded case')
        seen.add(key)
        before = bytes([row['seed']] * 33)
        require(row['before'] == before.hex() and
                row['after'] == (before[:16] + b'\0' + before[17:]).hex(), 'Exact guarded output')
        require(row['direct'] == 0 and row['property_index'] == 21 and row['call_kind'] == 1
                and row['guard_bytes'] == 16, 'Constant getter and dispatch contract')
    require(seen == set(itertools.product((0, 85, 255), ('0x0', '0x1', '0xffffffffffffffff'))),
            'Complete guarded matrix')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    report_path = args.root / 'probe/availability.json'
    report = json.loads(report_path.read_text(encoding='utf-8-sig'))
    validate(report)
    run = json.loads((args.root / 'probe/run.json').read_text(encoding='utf-8-sig'))
    require(run['completed'] is True and run['exit_code'] == 0 and 0 < run['elapsed_ms'] < 30000,
            'Bounded original child completion')
    repo = Path(__file__).resolve().parents[1]
    probe = repo / 'tools/probe_original_debug_availability.py'
    require(run['source_sha256'] == identity(probe)['sha256'], 'Executed probe identity')
    static = args.root / 'static'
    manifest = json.loads((static / 'evidence-sha256.json').read_text(encoding='utf-8-sig'))
    require({r['name'] for r in manifest} == {'binary.json', 'presenter-dispatch.txt',
            'presenter-dispatch.log', 'presenter-metadata.txt', 'presenter-metadata.log'}, 'Static inventory')
    for row in manifest:
        require(identity(static / row['name'])['sha256'] == row['sha256'], 'Static export digest')
    binary = json.loads((static / 'binary.json').read_text(encoding='utf-8-sig'))
    require(binary['sha256'] == PINNED and binary['version'] == '25, 1, 26603, 47871', 'Original binary version')
    for name in ('presenter-dispatch', 'presenter-metadata'):
        export = (static / (name + '.txt')).read_text(encoding='utf-8-sig')
        require(export.startswith(f'PROGRAM framedb_qt.dll SHA256 {PINNED} BASE 180000000\n')
                and export.rstrip().endswith('COMPLETE 3'), 'Complete pinned Ghidra export')
    assembly = (static / 'presenter-dispatch.txt').read_text(encoding='utf-8-sig')
    for instruction in ('0xb69bf JMP 0x180118980', '0xb6fc3 CALL 0x1800d22e0',
                        '0xb6fc8 MOV byte ptr [RBX],AL', '0xd22e0 XOR AL,AL', '0xd22e2 RET',
                        '0x1189b4 MOV RDX,qword ptr [RBX + 0x38]',
                        '0x1189b8 MOV RCX,qword ptr [RAX + 0x98]', '0x1189bf CALL 0x1802a3860'):
        require('ASM RVA ' + instruction + '\n' in assembly, 'Actual dispatch instruction: ' + instruction)
    controls = []
    for kind in ('binary', 'scope', 'getter', 'property', 'method', 'missing', 'duplicate',
                 'return', 'property_index', 'call_kind', 'guard', 'written_byte'):
        changed = copy.deepcopy(report)
        if kind == 'binary': changed['sha256'] = '0' * 64
        elif kind == 'scope': changed['engine_recovered'] = True
        elif kind == 'getter': changed['metadata']['getter_bytes'] = 'b001c3'
        elif kind == 'property': changed['metadata']['properties'][21]['name'] = 'shaderType'
        elif kind == 'method': changed['metadata']['methods'][23]['name'] = 'applyChanges'
        elif kind == 'missing': changed['cases'].pop()
        elif kind == 'duplicate': changed['cases'].append(changed['cases'][0])
        elif kind == 'return': changed['cases'][0]['direct'] = 1
        elif kind == 'property_index': changed['cases'][0]['property_index'] = 20
        elif kind == 'call_kind': changed['cases'][0]['call_kind'] = 0
        else:
            value = bytearray.fromhex(changed['cases'][0]['after'])
            value[0 if kind == 'guard' else 16] = 1
            changed['cases'][0]['after'] = value.hex()
        try:
            validate(changed)
        except ValueError as error:
            controls.append(dict(change=kind, reason=str(error)))
        else:
            raise ValueError('Corrupted availability evidence accepted')
    sources = [probe, Path(__file__), repo / 'tools/ghidra/Invoke-DebuggerAvailabilityResearch.ps1',
               repo / 'tools/ghidra/ExportFunctionEvidence.java', repo / 'tools/ghidra/ExportAddressEvidence.java']
    ledger = dict(schema=1, scope='original_shader_debug_availability_property', passed=True,
                  guarded_cases=9, negative_controls=controls, debugger_engine_recovered=False,
                  full_debugger_parity=False, renderdoc_removed=False,
                  files=[identity(p) for p in sorted(args.root.rglob('*')) if p.is_file()],
                  sources=[identity(p) for p in sources])
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(ledger, stream, indent=2); stream.write('\n')
    print('Validated nine original property reads, static dispatch evidence and twelve corruptions')


if __name__ == '__main__':
    main()
