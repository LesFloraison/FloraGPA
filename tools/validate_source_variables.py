"""Development-only CodeView symbols/types and native snapshot value parity."""
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

p = argparse.ArgumentParser(description=__doc__)
for name in ('reference', 'exe', 'qt-bin', 'out'):
    p.add_argument('--'+name, type=Path, required=True)
p.add_argument('--captures', type=Path)
a = p.parse_args()
a.out = a.out.resolve(); a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference/'standalone'), str(a.reference/'tools')]
from native_source_variables import decode, resolve, numeric, Types, PRIMITIVES
from shader_project import compile_project, FORMAT
from shaders import chunks
from shader_sources import PDB
from dxbc_patch import container
from validate_gs_source_lines import PROJECT
from validate_native_source_variables import TYPED
from source_variable_fixture import project as pixel_project
from validate_ds_checkpoints import project as domain_project
from probe_hs_source_scopes import project as hull_project
from gs_checkpoint import headers, register_values

jobs, expected = [], []
def job(name, op, oracle, **kwargs):
    jobs.append(dict(name=name, op=op, **kwargs))
    try:
        expected.append(dict(success=True, value=json.loads(json.dumps(oracle()))))
    except (ValueError, KeyError, UnicodeError, struct.error, IndexError) as exc:
        expected.append(dict(success=False, error=str(exc)))

def source(name, raw):
    path = a.out/(name+'.dxbc'); path.write_bytes(raw)
    job(name, 'source-variables', lambda: decode(raw), input=str(path))

projects = [('inline', PROJECT), ('pixel', pixel_project())]
typed = dict(format=FORMAT, files=[dict(name='types.hlsl', text=TYPED)], root='types.hlsl',
             entry='main', profile='gs_5_0', flags=5)
projects.append(('types', typed))
unused = copy.deepcopy(typed)
unused['files'][0]['text'] = TYPED.replace('a[(prim+gi)%3].x+a[(prim+gi)%3].y+a[(prim+gi)%3].z', 'a[(prim+gi)%3].x')
projects.append(('unused', unused))
double = copy.deepcopy(PROJECT)
double['files'][0]['text'] = double['files'][0]['text'].replace('next_value(value)', 'next_value(next_value(value))')
projects.append(('double-inline', double))
for flags in (1, 5, 2049):
    projects.extend([('hs-'+str(flags), hull_project(flags)), ('ds-'+str(flags), domain_project(flags=flags))])
for name, project in projects:
    raw, _ = compile_project(project); source(name, raw)
raw, _ = compile_project(typed)
parts = chunks(raw)
source('stripped', container({k:v for k,v in parts.items() if k != 'SPDB'}))
for length in (0, 1, 31, 55, 56, 511, len(parts['SPDB'])-1):
    source('truncated-'+str(length), container(dict(parts, SPDB=parts['SPDB'][:length])))
pdb = PDB(parts['SPDB']); dbi = pdb.stream(3)
stream = struct.unpack_from('<H', dbi, 98)[0]; symbols = pdb.stream(stream)
cursor = 4; records = []
while cursor < struct.unpack_from('<I', dbi, 100)[0]:
    length, kind = struct.unpack_from('<HH', symbols, cursor)
    records.append((cursor, kind)); cursor += length+2
def mutate(name, index, offset, data):
    changed = bytearray(parts['SPDB'])
    pages = pdb.streams[index][0]
    for i, byte in enumerate(data):
        logical = offset+i
        changed[pages[logical//pdb.page]*pdb.page+logical%pdb.page] = byte
    source(name, container(dict(parts, SPDB=bytes(changed))))
for n, (offset, kind) in enumerate(records):
    if kind == 0x1150:
        for field, relative, value in [('start', 8, struct.pack('<I', 0xffffffff)),
                                       ('segment', 12, struct.pack('<H', 2)),
                                       ('length', 14, struct.pack('<H', 1)),
                                       ('parent', 4, struct.pack('<H', 0xffff))]:
            mutate('range-'+str(n)+'-'+field, stream, offset+4+relative, value)
    if kind in (0x110f, 0x1110, 0x1146, 0x1147, 0x1103, 0x114d, 0x115d):
        mutate('scope-end-'+str(n), stream, offset+8, struct.pack('<I', offset))
for field, offset, value in [('version', 0, 0), ('head', 4, 55), ('count', 12, 0x110001), ('size', 16, 0xffffffff)]:
    mutate('type-stream-'+field, 2, offset, struct.pack('<I', value))

for tag, fmt in [(0x8000,'b'), (0x8001,'h'), (0x8002,'H'), (0x8003,'i'), (0x8004,'I'), (0x8009,'q'), (0x800a,'Q')]:
    bits = struct.calcsize(fmt)*8
    for value in [0, (1 << (bits-(fmt.islower())))-1] + ([-1] if fmt.islower() else []):
        data = struct.pack('<H'+fmt, tag, value)
        job(f'numeric-{tag}-{value}', 'codeview-numeric', lambda: numeric(data,0), bytes=list(data))
    for size in range(len(data)):
        truncated = data[:size]
        job(f'numeric-{tag}-truncate-{size}', 'codeview-numeric', lambda: numeric(truncated,0), bytes=list(truncated))
for value in (0, 1, 0x7fff, 0x8010):
    data = struct.pack('<H',value)
    job('numeric-short-'+str(value),'codeview-numeric',lambda:numeric(data,0),bytes=list(data))

def typ(name, recs, index=0x1000):
    model = Types.__new__(Types); model.records = recs; model.cache = {}
    job(name, 'source-type', lambda:model.get(index), index=index,
        records=[dict(id=i,kind=kind,bytes=list(data)) for i,(kind,data) in recs.items()])
for index in [*PRIMITIVES, 0x42, 0xffff]: typ('primitive-'+str(index),{},index)
for kind in (0x1001,0x1518):
    typ('alias-'+str(kind),{0x1000:(kind,struct.pack('<I',0x40))})
    typ('recursive-'+str(kind),{0x1000:(kind,struct.pack('<I',0x1000))})
for kind in (0x151b,0x1503,0x1516):
    for count in (0,1,3,4,5,4096,4097):
        for stride in (0,4,16):
            size = max(0,(count-1)*stride+4)
            data = struct.pack('<II',0x40,count if kind==0x151b else 0x74)
            if kind==0x1516:data+=struct.pack('<I',stride)
            data += struct.pack('<HI',0x8004,size)+b'items\0'
            typ(f'array-{kind}-{count}-{stride}',{0x1000:(kind,data)})
for rows in range(5):
    for cols in range(5):
        for flags in (0,1,2):
            data=struct.pack('<4IBH',0x40,rows,cols,16,flags,64)+b'matrix\0'
            typ(f'matrix-{rows}-{cols}-{flags}',{0x1000:(0x151c,data)})
fields=struct.pack('<HHIH',0x150d,0,0x40,0)+b'x\0'+b'\xf2\xf1'+struct.pack('<HHIH',0x150d,0,0x74,4)+b'y\0'
for size in (4,8):
    data=struct.pack('<HHIIIH',2,0,0x1001,0,0,size)+b'Pair\0'
    recs={0x1000:(0x1505,data),0x1001:(0x1203,fields)}
    typ('struct-'+str(size),recs)
    for end in range(len(fields)):
        typ(f'struct-{size}-truncated-{end}',{**recs,0x1001:(0x1203,fields[:end])})

def resolved(name, model, regs, meta, hit, offset=16):
    job(name,'resolve-source-variables',lambda:resolve(model,regs,meta,hit,offset),
        model=model,registers=regs,metadata=meta,hit=hit,offset=offset)
def scalar(kind='float',size=4,binding=None):
    return dict(scopes=[dict(id='s',name='main',kind='function')],variables=[dict(
        id='v',scope='s',name='value',type=dict(name=kind,size=size,
        leaves=[dict(path='',offset=0,type=kind,size=size)]),ranges=[binding or dict(
        register_type=0,flags=1,variable_offset=0,size=size,start=8,end=32,gaps=[],register_offsets=[0])])])
def reg(name,bits,written=None):
    return dict(name=name,bits=bits,written=written or [True]*4)
rng=random.Random(94305)
for kind,size in [('float',4),('double',8),('int',4),('int64',8),('uint',4),('uint64',8),('bool',4)]:
    values=[0,1,1<<(size*8-1),(1<<(size*8))-1]
    values += [rng.getrandbits(size*8) for _ in range(500)]
    if kind in ('float','double'):
        values += [int.from_bytes(struct.pack('<'+('f' if size==4 else 'd'),x),'little') for x in
                   [0.,-0.,float('inf'),-float('inf'),float('nan'),1e-5,1e-4,1e15,1e16]]
    for n,bits in enumerate(values):
        resolved(f'value-{kind}-{n}',scalar(kind,size),[reg('r0',[bits&0xffffffff,bits>>32,0,0])],{}, {})
base=scalar('uint'); r=base['variables'][0]['ranges'][0]
for offset in (0,8,15,16,19,20,31,32):
    for gap in ([],[[8,4]]):
        model=copy.deepcopy(base); model['variables'][0]['ranges'][0]['gaps']=gap
        resolved(f'gap-{offset}-{gap}',model,[reg('r0',[7,8,9,10])],{}, {},offset)
for flag in range(16):
    model=copy.deepcopy(base);model['variables'][0]['ranges'][0]['flags']=flag
    resolved('flag-'+str(flag),model,[reg('r0',[7,8,9,10])],{}, {})
for written in ([True]*4,[False]*4,[False,True,True,True]):
    resolved('written-'+str(written),base,[reg('r0',[7,8,9,10],written)],{}, {})
for other in (7,8):
    model=copy.deepcopy(base);model['variables'][0]['ranges'].append(dict(r,register_offsets=[4]))
    resolved('alias-conflict-'+str(other),model,[reg('r0',[7,other,0,0])],{}, {})
for kind,indices,name in [(0,[0],'r0'),(1,[0,2],'v[2][0]'),(2,[0],'o0'),
                           (11,[],'unused'),(37,[],'unused'),(25,[0,2],'vicp[2][0]'),
                           (26,[0,2],'vocp[2][0]'),(27,[0],'vpc0'),(28,[0xffffffc0],'vDomain'),
                           (22,[0xffffffd0],'vOutputControlPointID')]:
    for stage in ('gs','hs','ds'):
        for depth in (0,1):
            binding=dict(r,register_type=kind,register_offsets=indices)
            model=scalar('uint',binding=binding)
            model['hs_phases']=[dict(id=0,start=8,end=32,scope_ids=['s'])]
            meta=dict(shader_stage=stage,hs_phase=dict(id=0,kind='control_points'))
            resolved(f'input-{kind}-{stage}-{depth}',model,[reg(name,[11,12,13,14])],meta,
                     dict(call_depth=depth,primitive_id=3,gs_instance=2))
for components in (1,2,3,4):
    model=scalar('float');model['variables'][0]['type'].update(array=True,size=32,leaves=[
        dict(path=f'[{i}].{c}',offset=i*16+n*4,size=4,type='float') for i in range(2) for n,c in enumerate('xyzw')])
    model['variables'][0]['ranges']=[dict(r,register_type=3,register_offsets=[0],size=32)]
    resolved('indexable-'+str(components),model,[reg('x0[0]',[0x3f800000]*4),reg('x0[1]',[0x40000000]*4)],
             dict(indexable_temporaries=[dict(array=0,elements=2,components=components)]),{})

capture_count=0
if a.captures:
    for path in sorted(a.captures.glob('spdb-*-native/checkpoint.json')):
        report=json.loads(path.read_text());raw=(path.parent/'shader.dxbc').read_bytes();model=decode(raw)
        source(path.parent.name+'-symbols',raw)
        if 'register_capture' not in report:continue
        # HS phase ownership is supplied by the reference stack model for resolver-only
        # parity. Native production attachment remains a separately documented gap.
        if report['register_capture']['shader_stage']=='hs':
            from native_source_stack import build
            build(raw,model)
        data=(path.parent/'snapshots.bin').read_bytes();meta=report['register_capture']
        entries={r['token']:r['word_offset']*4 for r in report['catalog']}
        for row in headers(data,meta):
            token=row['token'] if meta.get('trace') else meta['checkpoint']['token']
            resolved(path.parent.name+'-record-'+str(row['record']),model,
                     register_values(data,meta,row['record']),meta,row,entries[token])
            capture_count+=1

manifest=a.out/'jobs.json';manifest.write_text(json.dumps(jobs),encoding='utf-8')
(a.out/'expected.json').write_text(json.dumps(expected),encoding='utf-8')
env=dict(os.environ);env['PATH']=str(a.qt_bin)+os.pathsep+env.get('PATH','')
subprocess.run([str(a.exe.resolve()),'--probe',str(manifest)],env=env,check=True)
actual=json.loads(Path(str(manifest)+'.results.json').read_text())
assert len(actual)==len(expected)
checks=[]
def semantic(value):
    if isinstance(value,dict):
        return {k:([bool(s) for s in v] if k=='issues' else bool(v) if k in ('type_issue','issue') else semantic(v)) for k,v in value.items()}
    if isinstance(value,list):return [semantic(v) for v in value]
    return value
for case,want,got in zip(jobs,expected,actual):
    exact=want['success']==got['success'] and (not want['success'] or want['value']==got['value'])
    equivalent=want['success']==got['success'] and (not want['success'] or semantic(want['value'])==semantic(got['value']))
    checks.append(dict(name=case['name'],passed=equivalent,exact=exact))
report=dict(passed=all(r['passed'] for r in checks),count=len(checks),exact=sum(r['exact'] for r in checks),
            snapshot_records=capture_count,checks=checks,
            pending=['SDBG assignments','production HS phase attachment','Qt source variable view'],
            executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest())
(a.out/'validation.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in report.items() if k!='checks'}),flush=True)
for row in checks:
    if not row['passed']:print(row,flush=True)
raise SystemExit(0 if report['passed'] else 1)
