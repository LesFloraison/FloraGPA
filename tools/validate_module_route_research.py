"""Validate GPA loader research against raw PE exports, memory and Ghidra calls."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct

from probe_original_module_routes import PINNED, digest


def require(value, message):
    if not value:
        raise ValueError(message)


def exports(path):
    data = path.read_bytes()
    require(hashlib.sha256(data).hexdigest() == PINNED['ShaderParse.dll'], 'Parser identity')
    pe, = struct.unpack_from('<I', data, 0x3c)
    require(data[:2] == b'MZ' and data[pe:pe+4] == b'PE\0\0', 'PE signature')
    machine, count = struct.unpack_from('<HH', data, pe+4)
    optional, = struct.unpack_from('<H', data, pe+20)
    require(machine == 0x8664 and struct.unpack_from('<H', data, pe+24)[0] == 0x20b, 'PE64 layout')
    sections = [struct.unpack_from('<IIII', data, pe+24+optional+i*40+8) for i in range(count)]

    def offset(rva, size=1):
        matches = [raw+rva-start for virtual, start, length, raw in sections
                   if start <= rva and rva+size <= start+length and raw+rva-start+size <= len(data)]
        require(len(matches) == 1, 'File-backed PE range')
        return matches[0]

    directory, length = struct.unpack_from('<II', data, pe+24+112)
    functions, names, function_table, name_table, ordinals = struct.unpack_from('<IIIII', data, offset(directory+20, 20))
    require(0 < names <= functions < 100000, 'Export bounds')
    result = {}
    for i in range(names):
        name_rva, = struct.unpack_from('<I', data, offset(name_table+i*4, 4))
        start = offset(name_rva)
        end = data.find(b'\0', start, start+1024)
        require(end >= start, 'Terminated export name')
        name = data[start:end].decode('ascii')
        ordinal, = struct.unpack_from('<H', data, offset(ordinals+i*2, 2))
        require(ordinal < functions, 'Export ordinal')
        rva, = struct.unpack_from('<I', data, offset(function_table+ordinal*4, 4))
        require(not directory <= rva < directory+length, 'Direct parser export expected')
        offset(rva)
        result[name] = rva
    return result


def validate_probe(report, endpoints):
    require(report['completed'] is True and report['engine_recovered'] is False and
            report['shader_executed'] is False and report['shim_active'] is False and
            report['parser_preloaded_by_absolute_path'] is True, 'Explicit research scope')
    require(report['binary_sha256'] == PINNED, 'Pinned binary identities')
    require(report['singleton_types'] == {'0x1fd240': '.PEAVIGPALogger@@', '0x1fd268': '.?AVCLog@@'},
            'Actual singleton type strings')
    require(len(report['rows']) == 6 and len(report['resolutions']) == 6, 'Complete probe matrix')
    seen = set()
    for row in report['rows']:
        pattern, marker = row['pattern'], int(row['marker'], 16)
        require(pattern in (0xa5, 0x5a) and marker in (0, 0x123456789abcdef0, 0xfedcba9876543210), 'Owned case')
        require((pattern, marker) not in seen, 'Unique owned case')
        seen.add((pattern, marker))
        initial = bytes([pattern])*136
        require(bytes.fromhex(row['initial']) == initial, 'Initial guard pattern')
        require(row['pointer'] == row['returned'] and int(row['pointer'], 16) != 0, 'Constructor return')
        module = int(row['module_base'], 16)
        require(module == int(report['parser_base'], 16) and module != 0, 'Pinned preloaded module')
        require(Path(row['module_path']).name.lower() == 'shaderparse.dll' and row['module_name'] == 'ShaderParse.dll',
                'Bound module name')
        expected = bytearray(initial)
        struct.pack_into('<QQ', expected, 32, marker, module)
        expected[48:64] = b'ShaderParse.dll\0'
        struct.pack_into('<QQ', expected, 64, 15, 15)
        for i, (name, slot) in enumerate(zip(('Assemble', 'GetAsmParseError', 'Release'), row['endpoints'])):
            require(slot['name'] == name and int(slot['rva'], 16) == endpoints[name] and
                    int(slot['address'], 16) == module+endpoints[name], 'PE-backed endpoint')
            struct.pack_into('<Q', expected, 80+i*8, module+endpoints[name])
        require(len(row['endpoints']) == 3 and bytes.fromhex(row['constructed']) == expected, 'Complete constructed memory')
        struct.pack_into('<Q', expected, 40, 0)
        require(bytes.fromhex(row['unloaded']) == expected and row['released'] is True and row['second_release'] is False,
                'Reference release only clears the handle')
        expected[48] = 0
        struct.pack_into('<QQ', expected, 64, 0, 15)
        require(bytes.fromhex(row['cleaned']) == expected, 'Complete cleanup memory')
    names = set()
    for row in report['resolutions']:
        handle = int(row['handle'], 16)
        key = row['name'], bool(handle)
        require(key not in names, 'Unique resolver case')
        names.add(key)
        require(row['actual'] == row['expected'] and (int(row['actual'], 16) != 0) ==
                (bool(handle) and row['name'] != 'FloraMissingResearchProcedure'), 'Windows resolver comparison')
        require(row['holder_before'] == row['holder_after'] and row['argument_before'] == row['argument_after'],
                'Resolver arguments remain unchanged')
    require(names == {(name, present) for name in ('GetTickCount','GetCurrentProcessId','FloraMissingResearchProcedure')
                     for present in (False, True)}, 'Resolver inline, heap, missing and null cases')
    require(len(report['directories']) == 2, 'Both directory origins')
    for rva, row in zip(('0xcea40', '0xce640'), report['directories']):
        require(row['rva'] == rva and row['returned'] == row['output'], 'Directory function and return')
        require((int(row['module'], 16) == 0) == (rva == '0xcea40'), 'Executable versus containing module')
        require(Path(row['text']).resolve() == Path(row['module_path']).parent.resolve(), 'Windows directory oracle')
        require(Path(row['module_path']).name.lower() == ('python.exe' if rva == '0xcea40' else 'dx11_player.dll'),
                'Observed research host')
        before = bytes.fromhex(row['before_cleanup'])
        require(len(before) == 104 and before[:40] == b'\xa5'*40 and before[-32:] == b'\xa5'*32, 'Directory guards')
        length, capacity = struct.unpack_from('<QQ', before, 56)
        require(length == len(row['text'].encode('utf-8')) and capacity >= length, 'Directory string length')
        expected = bytearray(before)
        expected[40] = 0
        struct.pack_into('<QQ', expected, 56, 0, 15)
        require(bytes.fromhex(row['cleaned']) == expected, 'Directory cleanup bytes')


COUNTS = {'loader-symbols': 5, 'procedure-symbols': 2, 'loaders': 2, 'loader-callers': 12,
          'resolvers': 10, 'singleton-types': 2, 'extension-symbols': 77, 'extension-table': 1,
          'module-lifetime': 3, 'holder-cleanup': 1, 'endpoint-strings': 7, 'directory-selection': 2}
EDGES = {
    'loaders': ['ASM RVA 0xc791f CALL qword ptr [0x180174208]', 'ASM RVA 0x14f0a2 CALL qword ptr [0x180174480]'],
    'loader-callers': ['ASM RVA 0xface6 CALL 0x1800c7290', 'ASM RVA 0xfad85 MOV qword ptr [RBX + 0x30],RAX',
                      'ASM RVA 0xfae16 MOV qword ptr [RBX + 0x38],RAX', 'ASM RVA 0xfed0b CALL 0x1800c7290'],
    'resolvers': ['ASM RVA 0x11e44a LEA RCX,[0x1801fd238]', 'ASM RVA 0x11d918 LEA RCX,[0x1801fd260]',
                  'ASM RVA 0xc670f CALL qword ptr [0x180174118]'],
    'extension-table': ['SYMBOL MetricsFramework::ExtensionLoader::vftable', 'QWORD +0x8 18014e730', 'QWORD +0x10 18014d690'],
    'module-lifetime': ['ASM RVA 0xc7f61 MOV qword ptr [RBX],0x0'],
    'holder-cleanup': ['ASM RVA 0x38510 MOV qword ptr [RBX + 0x18],0x0'],
    'directory-selection': ['ASM RVA 0xcea88 XOR ECX,ECX', 'ASM RVA 0xcea8a CALL qword ptr [0x180174250]',
                            'ASM RVA 0xce687 LEA RDX,[0x1800ce640]', 'ASM RVA 0xce692 CALL qword ptr [0x180174278]'],
}


def static_evidence(texts):
    for name, count in COUNTS.items():
        text = texts[name]
        require(text.startswith('PROGRAM dx11_player.dll SHA256 '+PINNED['dx11_player.dll']+' BASE 180000000\n') and
                text.rstrip().endswith('COMPLETE '+str(count)) and '\nNO FUNCTION ' not in text, 'Complete Ghidra export: '+name)
        for edge in EDGES.get(name, []):
            require(edge in text, 'Missing disassembly/call evidence: '+edge)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--gpa', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    endpoints = exports(args.gpa/'ShaderParse.dll')
    for name, sha in PINNED.items():
        require(digest(args.gpa/name) == sha, 'Installed binary identity')
    root = args.directory
    report = json.loads((root/'probe-complete/probe.json').read_text(encoding='utf-8'))
    process = json.loads((root/'probe-complete/process.json').read_text(encoding='utf-8'))
    require(process['completed'] is True and process['exit_code'] == 0 and process['timed_out'] is False and
            process['deadline_seconds'] == 30, 'Bounded child completion')
    require(report['source_sha256'] == process['source_sha256'] == digest(Path('tools/probe_original_module_routes.py')),
            'Current probe source identity')
    validate_probe(report, endpoints)
    texts = {n: (root/'verified-final'/f'{n}.txt').read_text(encoding='utf-8') for n in COUNTS}
    static_evidence(texts)
    controls = 0
    mutations = []
    for i in range(6):
        mutations += [lambda r, i=i: r['rows'][i].update(constructed='00'*136),
                      lambda r, i=i: r['rows'][i].update(cleaned='00'*136),
                      lambda r, i=i: r['rows'][i]['endpoints'][0].update(rva='0x0'),
                      lambda r, i=i: r['rows'][i].update(returned='0x0'),
                      lambda r, i=i: r['rows'][i].update(second_release=True),
                      lambda r, i=i: r['resolutions'][i].update(expected='0x1234')]
    mutations += [lambda r: r.update(engine_recovered=True), lambda r: r.update(shim_active=True),
                  lambda r: r['singleton_types'].update({'0x1fd268': 'Debugger'}),
                  lambda r: r['directories'][0].update(text='C:/WrongResearchDirectory'),
                  lambda r: r['directories'][1].update(cleaned='00'*104)]
    for mutate in mutations:
        altered = copy.deepcopy(report)
        mutate(altered)
        try:
            validate_probe(altered, endpoints)
        except (ValueError, KeyError):
            controls += 1
        else:
            raise ValueError('Corrupt module-route observation accepted')
    for name, edges in EDGES.items():
        altered = dict(texts)
        altered[name] = altered[name].replace(edges[0], 'REMOVED')
        try:
            static_evidence(altered)
        except ValueError:
            controls += 1
        else:
            raise ValueError('Missing static route accepted')
    paths = {p for p in root.rglob('*') if p.is_file()}
    paths.update(Path(p) for p in ('tools/probe_original_module_routes.py','tools/validate_module_route_research.py',
        'tools/ghidra/Invoke-ModuleRouteResearch.ps1','tools/ghidra/ExportFunctionEvidence.java',
        'tools/ghidra/ExportSymbolEvidence.java','tools/ghidra/ExportAddressEvidence.java'))
    ledger = dict(schema=1, scope='gpa_module_loader_and_parser_binding_routes', engine_recovered=False,
        shader_executed=False, renderdoc_removed=False, binary_sha256=PINNED, constructor_cases=6,
        resolver_cases=6, directory_cases=2, ghidra_exports=len(texts), negative_controls=controls, elapsed_ms=process['elapsed_ms'],
        parser_exports={name: hex(rva) for name,rva in endpoints.items()},
        evidence=[dict(path=p.as_posix(),bytes=p.stat().st_size,sha256=digest(p)) for p in sorted(paths)])
    with args.out.open('x', encoding='utf-8') as stream:
        json.dump(ledger, stream, indent=2); stream.write('\n')
    print(json.dumps({key:ledger[key] for key in ('constructor_cases','resolver_cases','ghidra_exports','negative_controls','elapsed_ms')}))


if __name__ == '__main__':
    main()
