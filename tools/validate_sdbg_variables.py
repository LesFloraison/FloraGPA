"""Development-only legacy symbol and assignment-history parity against Python."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
from unittest.mock import patch

p = argparse.ArgumentParser(description=__doc__)
for name in ('reference', 'exe', 'qt-bin', 'out', 'captures'):
    p.add_argument('--'+name, type=Path, required=True)
p.add_argument('--history-only', action='store_true', help='Validate existing captured traces without recompiling mutation fixtures')
a = p.parse_args()
a.out = a.out.resolve(); a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference/'standalone'), str(a.reference/'tools')]
from native_sdbg_variables import decode, TraceValues
from sdbg_tess_fixture import compile_legacy
from sdbg_variable_fixture import SOURCE, TYPED
from sdbg_array_fixture import SOURCE as ARRAY
from probe_sdbg_wide import PREFIX, CASES
from shader_project import FORMAT
from shader_sdbg import SDBG
from shaders import chunks, disassemble
from dxbc_patch import container
from gs_checkpoint import headers, register_values

jobs, expected = [], []
def job(name, op, oracle, **kwargs):
    jobs.append(dict(name=name, op=op, **kwargs))
    try:
        expected.append(dict(success=True, value=json.loads(json.dumps(oracle()))))
    except (ValueError, KeyError, IndexError, UnicodeError, struct.error, StopIteration) as exc:
        expected.append(dict(success=False, error=str(exc)))

def symbols(name, raw, assembly=None):
    path = a.out/(name+'.dxbc'); path.write_bytes(raw)
    def oracle():
        if assembly is None:return decode(raw)
        # Mutations alter only SDBG. Keep the verified original code assembly;
        # the platform compiler itself can crash on malformed debug records.
        with patch('native_sdbg_variables.disassemble', return_value=assembly):return decode(raw)
    job(name, 'sdbg-variables', oracle, input=str(path))

sources = [('basic', SOURCE), ('typed', TYPED), ('array', ARRAY)]
for name, (body, expression) in CASES.items():
    sources.append(('wide-'+name, PREFIX+body+'\nV o;o.p=input[0];o.ids=uint4('+expression+',prim,0,1);dst.Append(o);\n}'))
raws, compilation_failures = [], []
for name, text in ([] if a.history_only else sources):
    for flags in (1, 5):
        project = dict(format=FORMAT, files=[dict(name='fixture.hlsl', text=text)],
                       root='fixture.hlsl', entry='main', profile='gs_5_0', flags=flags)
        try: raw = compile_legacy(project)
        except ValueError as exc:
            compilation_failures.append(dict(name=name, flags=flags, error=str(exc)))
            continue
        raws.append((name+'-'+str(flags), raw))
        symbols(name+'-'+str(flags), raw)

# Exercise actual compiler tables, including assignment masks, dynamic storage,
# shapes, scopes, inputs and duplicate identities. Diagnostic wording is tracked
# separately from semantic parity; unavailable data must never become available.
for name, raw in raws:
    parts = chunks(raw); data = parts['SDBG']; s = SDBG(data)
    assembly = disassemble(raw, 0xa4)
    for size in (0, 1, 83, 84, len(data)-1):
        symbols(name+'-truncate-'+str(size), container(dict(parts, SDBG=data[:size])), assembly)
    for table, (count, offset, width) in s.tables.items():
        for index in sorted(set([0, count//2, count-1])) if count else []:
            for field in range(width//4):
                for value in (0, 0xffffffff):
                    changed = bytearray(data)
                    struct.pack_into('<I', changed, 84+offset+index*width+4*field, value)
                    symbols(f'{name}-{table}-{index}-{field}-{value}', container(dict(parts, SDBG=bytes(changed))), assembly)

records = 0
rng = random.Random(8137)
def history(name, report, rows, registers, indices, shader=None):
    def oracle():
        trace = TraceValues(report, rows, lambda i: registers[i])
        return dict(values=[trace.at(i) for i in indices], events=trace.count)
    args = dict(result=report, rows=rows, registers=registers, indices=indices)
    if shader is not None: args['input'] = str(shader)
    job(name, 'sdbg-history', oracle, **args)

for path in sorted(a.captures.glob('sdbg-*-trace-native/checkpoint.json')):
    report = json.loads(path.read_text()); raw = (path.parent/'shader.dxbc').read_bytes()
    symbols(path.parent.name, raw)
    report['source_variables'] = decode(raw)
    assert report['source_variables']['status'] == 'available', path
    meta = report['register_capture']; data = (path.parent/'snapshots.bin').read_bytes()
    rows = headers(data, meta); entries = {e['token']:e for e in report['catalog']}
    for row in rows: row['instruction'] = entries[row['token']]['instruction']
    registers = [register_values(data, meta, row['record']) for row in rows]
    records += len(rows)
    indices = list(range(len(rows))) + list(reversed(range(len(rows))))
    shuffled = list(range(len(rows))); rng.shuffle(shuffled); indices += shuffled
    history(path.parent.name, report, rows, registers, indices, path.parent/'shader.dxbc')
    # Use a complete first invocation for focused failure and validity cases.
    end = next((i for i, row in enumerate(rows) if row['invocation'] != rows[0]['invocation']), len(rows))
    small, regs = rows[:end], registers[:end]
    for field, value in [('token', 0xffffffff), ('opcode', 0xffffffff), ('instruction', 0xffffffff), ('hit', 99), ('hs_phase', 999)]:
        broken = copy.deepcopy(small); broken[0][field] = value
        history(path.parent.name+'-wrong-'+field, report, broken, regs, [0])
    history(path.parent.name+'-missing-entry', report, small[1:], regs[1:], [0])
    if end < len(rows):
        broken = copy.deepcopy(small + [rows[end]] + small)
        history(path.parent.name+'-noncontiguous-invocation', report, broken, regs+[registers[end]]+regs, [0])
    if len(small)>2:
        broken=copy.deepcopy(small); broken[1]['hit']+=1
        history(path.parent.name+'-gap',report,broken,regs,[1])
    for mode in ('missing-registers', 'unwritten', 'call-depth', 'partial-double', 'unknown-array-index'):
        changed = copy.deepcopy(regs); broken = copy.deepcopy(small)
        if mode=='missing-registers': changed=[[] for _ in regs]
        elif mode=='unwritten':
            for rs in changed:
                for r in rs:r['written']=[False]*4
        elif mode=='call-depth':
            for r in broken:r['call_depth']=1
        elif mode=='partial-double':
            for rs in changed:
                for r in rs:r['written']=[r['written'][0],False,r['written'][2],False]
        else:
            for rs in changed:
                for r in rs:
                    if r['name'].startswith('r'):r['written']=[False]*4
        history(path.parent.name+'-'+mode, report, broken, changed, list(range(end)))
    for field, value in [('trace',False),('shader_sha256','mismatch')]:
        broken = copy.deepcopy(report); broken[field]=value
        history(path.parent.name+'-report-'+field,broken,small,regs,[0])
assert jobs and records, 'No SDBG trace fixtures were validated'

manifest=a.out/'jobs.json';manifest.write_text(json.dumps(jobs),encoding='utf-8')
(a.out/'expected.json').write_text(json.dumps(expected),encoding='utf-8')
env=dict(os.environ);env['PATH']=str(a.qt_bin)+os.pathsep+env.get('PATH','')
subprocess.run([str(a.exe.resolve()),'--probe',str(manifest)],env=env,check=True)
actual=json.loads(Path(str(manifest)+'.results.json').read_text())
assert len(actual)==len(expected)
def semantic(value):
    if isinstance(value,dict):
        return {k:([bool(s) for s in v] if k=='issues' else bool(v) if k in ('type_issue','issue') else semantic(v)) for k,v in value.items()}
    if isinstance(value,list):return [semantic(v) for v in value]
    return value
checks=[]
for case,want,got in zip(jobs,expected,actual):
    exact=want['success']==got['success'] and (not want['success'] or want['value']==got['value'])
    equivalent=want['success']==got['success'] and (not want['success'] or semantic(want['value'])==semantic(got['value']))
    checks.append(dict(name=case['name'],passed=equivalent,exact=exact))
report=dict(passed=all(r['passed'] for r in checks),count=len(checks),exact=sum(r['exact'] for r in checks),
            compilation_failures=compilation_failures,
            snapshot_records=records,checks=checks,executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),
            reference_sha256=hashlib.sha256((a.reference/'standalone/native_sdbg_variables.py').read_bytes()).hexdigest())
(a.out/'validation.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in report.items() if k!='checks'}),flush=True)
for row in checks:
    if not row['passed']:print(row,flush=True)
raise SystemExit(0 if report['passed'] else 1)
