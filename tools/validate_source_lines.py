"""Development-only SPDB/SDBG line-map parity, including corrupted debug data."""
import argparse
import copy
import hashlib
import json
import ntpath
import os
import random
from pathlib import Path
import struct
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
for name in ('reference', 'exe', 'qt-bin', 'out'):
    p.add_argument('--'+name, type=Path, required=True)
a = p.parse_args()
a.out = a.out.resolve(); a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference/'standalone'), str(a.reference/'tools')]
from shader_lines import decode, by_offset, inlinee_sources
from shader_project import compile_project
from shaders import chunks, disassemble
from shader_sources import PDB, text_decode
from dxbc_patch import container
from validate_gs_source_lines import PROJECT
from validate_sdbg import legacy
from sdbg_tess_fixture import fixture as tess_fixture
from validate_ds_checkpoints import project as domain_project
from probe_hs_source_scopes import project as hull_project

jobs, expected = [], []
def job(name, op, expected_fn, **kwargs):
    jobs.append(dict(name=name, op=op, **kwargs))
    try: expected.append(dict(success=True, value=expected_fn()))
    except (ValueError, KeyError, UnicodeError, struct.error) as e:
        expected.append(dict(success=False, error=str(e)))

def source(name, raw, assembly=None):
    path = a.out/(name+'.dxbc'); path.write_bytes(raw)
    def oracle():
        report = decode(raw, assembly)
        return dict(report=report, by_offset={str(k): v for k, v in by_offset(report).items()})
    job(name, 'source-lines', oracle, input=str(path), **({'assembly': assembly} if assembly is not None else {}))

raw, _ = compile_project(PROJECT)
source('spdb-includes', raw)
parts = chunks(raw); source('stripped', container({k: v for k, v in parts.items() if k != 'SPDB'}))
source('unsupported-debug', container({**{k: v for k, v in parts.items() if k != 'SPDB'}, 'ILDB': b'unknown'}))
for flags in (1, 5, 2049):
    project = copy.deepcopy(PROJECT); project['flags'] = flags
    code, _ = compile_project(project); source('spdb-gs-flags'+str(flags), code)
    for stage, project in [('hs', hull_project(flags)), ('ds', domain_project(flags=flags))]:
        code, _ = compile_project(project); source('spdb-'+stage+'-flags'+str(flags), code)
assembly = disassemble(raw, 0xa4)
source('spdb-no-directives', raw, '\n'.join(line for line in assembly.splitlines() if not line.startswith('#line ')))
source('spdb-invalid-directive-offset', raw, '#line 1 "'+decode(raw)['files'][0]['name']+'"\n0 0x00000009: ret')
for size in (0, 1, 31, 55, 56, 511, len(parts['SPDB'])-1):
    source('spdb-truncate'+str(size), container(dict(parts, SPDB=parts['SPDB'][:size])))
digest = hashlib.md5(PROJECT['files'][0]['text'].encode()).digest()
assert parts['SPDB'].count(digest) == 1
source('spdb-checksum-mismatch', container(dict(parts, SPDB=parts['SPDB'].replace(digest, bytes([digest[0]^1])+digest[1:]))))
pdb = PDB(parts['SPDB']); dbi = pdb.stream(3)
stream = struct.unpack_from('<H', dbi, 98)[0]; symbols = struct.unpack_from('<I', dbi, 100)[0]
module = pdb.stream(stream); cur = symbols
sections = {}
while cur < len(module):
    if len(module)-cur < 8: break
    kind, size = struct.unpack_from('<II', module, cur)
    if cur+8+size > len(module): break
    sections.setdefault(kind, []).append((cur+8, size)); cur = (cur+8+size+3)&~3
def module_mutation(name, logical, value, fmt='<I'):
    data = bytearray(parts['SPDB']); replacement = struct.pack(fmt, value)
    for i, byte in enumerate(replacement):
        pos = logical+i
        physical = pdb.streams[stream][0][pos//pdb.page]*pdb.page + pos%pdb.page
        data[physical] = byte
    source(name, container(dict(parts, SPDB=bytes(data))))
start, size = sections[0xf2][0]
for name, offset, value, fmt in [('line-middle', 24, 1, '<I'), ('segment', 4, 2, '<H'),
                                ('flags', 6, 2, '<H'), ('contribution', 8, 0xffffffff, '<I'),
                                ('file-id', 12, 0xffffffff, '<I'), ('count', 16, 0xffffffff, '<I'),
                                ('block-size', 20, 0xffffffff, '<I')]:
    module_mutation('spdb-'+name, start+offset, value, fmt)
for kind in (0xf2, 0xf4):
    start, size = sections[kind][0]
    module_mutation('spdb-section-size'+str(kind), start-4, 0xffffffff)

text = '// Legacy original 中文 comments\r\nfloat4 pixelEntry():SV_Target {\r\n float4 color=float4(0,0,1,1);\r\n return color;\r\n}\r\n'
legacy_codes = []
for version in (40, 41, 42, 43):
    code = legacy(text, profile='ps_4_0' if version != 43 else 'ps_5_0', version=version)
    source('sdbg-compiler'+str(version), code); legacy_codes.append(code)
code = legacy('#include "colors.hlsl"\nfloat4 pixelEntry():SV_Target {return color();}\n', includes={'colors.hlsl': '// include\nfloat4 color(){return float4(0,1,0,1);}\n'})
source('sdbg-includes', code)
stages, _ = tess_fixture(a.out/'legacy-tess.gpa_frame')
for stage, code in stages.items(): source('sdbg-'+stage, code)
parts = chunks(legacy_codes[-1]); dbg = parts['SDBG']; h = struct.unpack_from('<21I', dbg)
for size in [*range(85), len(dbg)-1]:
    source('sdbg-truncate'+str(size), container(dict(parts, SDBG=dbg[:size])))
for offset in (0, 4, 8, 12, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80):
    data = bytearray(dbg); struct.pack_into('<I', data, offset, 0xffffffff)
    source('sdbg-header'+str(offset), container(dict(parts, SDBG=data)))
for relative, value, label in [(0, 99, 'id'), (4, 0, 'opcode'), (372, 0xfffffffe, 'token')]:
    data = bytearray(dbg); struct.pack_into('<I', data, 84+h[8]+relative, value)
    source('sdbg-wrong-'+label, container(dict(parts, SDBG=data)))
for relative, label in [(0, 'file'), (4, 'file-length'), (8, 'source'), (12, 'source-length')]:
    data = bytearray(dbg); struct.pack_into('<I', data, 84+h[6]+relative, 0xffffffff)
    source('sdbg-wrong-'+label, container(dict(parts, SDBG=data)))
for label, index in [('compiler', 1), ('entry', 2), ('profile', 3)]:
    data = bytearray(dbg); data[84+h[20]+h[index]] = 255
    source('sdbg-invalid-metadata-utf8-'+label, container(dict(parts, SDBG=data)))
file_row = struct.unpack_from('<4I', dbg, 84+h[6])
data = bytearray(dbg); data[84+h[20]+file_row[2]] = 255
source('sdbg-invalid-utf8', container(dict(parts, SDBG=data)))
for label, content in [('truncated-utf8', b'abc\xe2\x82'), ('odd-utf16', b'\xff\xfea\x00b'), ('utf16', '// unicode\r\n'.encode('utf-16'))]:
    data = bytearray(dbg); offset = len(data)-(84+h[20]); data.extend(content)
    struct.pack_into('<2I', data, 84+h[6]+8, offset, len(content))
    source('sdbg-'+label, container(dict(parts, SDBG=data)))
for i, separator in enumerate(['\n', '\r', '\r\n', '\v', '\f', '\x1c', '\x1d', '\x1e', '\x85', '\u2028', '\u2029']):
    for trailing in (False, True):
        content = separator.join(['a', '', 'b', 'c']) + (separator if trailing else '')
        data = bytearray(dbg); offset = len(data)-(84+h[20]); encoded = content.encode(); data.extend(encoded)
        struct.pack_into('<2I', data, 84+h[6]+8, offset, len(encoded))
        source('sdbg-line-separator-'+str(i)+'-'+str(trailing), container(dict(parts, SDBG=data)))
source('sdbg-invalid-disassembly', legacy_codes[-1], '0 0x00000009: ret')
source('sdbg-no-disassembly', legacy_codes[-1], '')
for name in ['', '.', '..', 'a/../b', 'C:/Temp/../Source/Main.HLSL', r'C:a\..\b', r'C:\..\a',
             r'\\host\share\a\..\b', r'\\?\C:\a\..\B', r'\\?\UNC\host\share\a\..\b',
             r'\a\..\..\b', 'Straße.HLSL', 'İ.hlsl', 'Σςσ.HLSL', '中文/./A.hlsl']:
    job('path-'+str(len(jobs)), 'source-path', lambda n=name: ntpath.normpath(n).casefold(), path=name)
for cp in range(0x110000):
    name = chr(cp)
    if name.casefold() != name:
        job('unicode-'+hex(cp), 'source-path', lambda n=name: ntpath.normpath(n).casefold(), path=name)
rng = random.Random(9021)
for i in range(2000):
    name = rng.choice(['', 'C:', 'C:/', '/', '//host/share/', '//?/C:/', '//?/UNC/host/share/']) + '/'.join(rng.choices(['a', 'B', '.', '..', '', 'Straße', 'İ'], k=rng.randrange(1, 9)))
    job('random-path-'+str(i), 'source-path', lambda n=name: ntpath.normpath(n).casefold(), path=name)
texts = [bytes([v]) for v in range(256)]
texts += [b'\xff\xfe'+bytes([v]) for v in range(256)]
texts += [b'\xfe\xff'+bytes([v]) for v in range(256)]
texts += [bytes.fromhex(v) for v in ['e282', 'f09080', 'fffe00d800', 'fffe00dc00', 'edbfbf', 'efbbbf', 'f48fbfbf']]
texts += [prefix + rng.randbytes(rng.randrange(30)) for prefix in (b'', b'\xff\xfe', b'\xfe\xff', b'\xef\xbb\xbf') for _ in range(500)]
for i, data in enumerate(texts):
    def expected_text(b=data):
        text, encoding, valid = text_decode(b)
        return dict(text=text, encoding=encoding, text_valid=valid)
    job('source-text-'+str(i), 'source-text', expected_text, bytes=list(data))
sample = dict(byte_offset=8, file=0, line_start=1, line_end=1, statement=True)
for i, rows in enumerate([[sample], [sample, dict(sample, line_start=2)],
                          [sample, dict(sample, statement=False, line_start=2, line_end=2)],
                          [dict(sample, statement=False), dict(sample, statement=False, file=1)], []]):
    report = dict(locations=rows)
    job('offsets'+str(i), 'source-offsets', lambda r=report: {str(k): v for k, v in by_offset(r).items()}, report=report)
checksums = {0: 0, 24: None}
tables = [struct.pack('<4I', 0, 123, 0, 7), struct.pack('<7I', 1, 124, 0, 8, 2, 0, 24)]
for i, selected in enumerate([[], tables, [tables[0], tables[0]], [b''], [struct.pack('<I', 2)],
                              [tables[1][:-1]], [struct.pack('<4I', 0, 123, 1, 7)],
                              [tables[0], struct.pack('<4I', 0, 123, 0, 8)]]):
    job('inlinees'+str(i), 'inlinee-sources', lambda t=selected: inlinee_sources(t, checksums),
        tables=[list(t) for t in selected], checksums={str(k): v for k, v in checksums.items()})

manifest = a.out/'jobs.json'; manifest.write_text(json.dumps(jobs), encoding='utf-8')
env = dict(os.environ); env['PATH'] = str(a.qt_bin.resolve())+os.pathsep+env['PATH']
run = subprocess.run([str(a.exe.resolve()), '--probe', str(manifest)], env=env, capture_output=True, timeout=120)
(a.out/'probe.log').write_bytes(run.stdout+run.stderr)
assert run.returncode == 0, run.stderr
actual = json.loads(Path(str(manifest)+'.results.json').read_text())
assert len(actual) == len(expected)
def diagnostics(value):
    if isinstance(value, dict):
        return {k: ([bool(s) for s in v] if k in ('issues', 'inlinee_issues') else diagnostics(v)) for k, v in value.items()}
    if isinstance(value, list): return [diagnostics(v) for v in value]
    return value
checks = []
for j, want, got in zip(jobs, expected, actual):
    exact = want['success'] == got['success'] and (not want['success'] or want['value'] == got['value'])
    passed = want['success'] == got['success'] and (not want['success'] or diagnostics(want['value']) == diagnostics(got['value']))
    checks.append(dict(name=j['name'], passed=passed, exact=exact, reference=want, actual=got))
    if not passed: print(j['name'], 'FAIL', flush=True)
result = dict(completed=True, passed=all(c['passed'] for c in checks), checks=checks,
              executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),
              sources={name: hashlib.sha256((a.reference/'standalone'/name).read_bytes()).hexdigest()
                       for name in ('shader_lines.py', 'shader_sdbg.py', 'shader_sources.py')})
(a.out/'validation.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(len(checks), 'checks;', sum(c['passed'] for c in checks), 'passed;', sum(c['exact'] for c in checks), 'exact')
raise SystemExit(0 if result['passed'] else 1)
