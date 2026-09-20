"""Development-only differential checks for native checkpoint parsing and selectors."""
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
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--qt-bin', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--real', action='store_true')
a = p.parse_args()
a.out = a.out.resolve()
a.out.mkdir(parents=True, exist_ok=False)
sys.path.insert(0, str(a.reference / 'standalone'))
import dxbc_indexable as arrays
import dxbc_domain_inputs as domain
import dxbc_hull_phases as hull
import dxbc_gs_checkpoint as checkpoint
import native_invocation_selector as selector
from dxbc_hull_capture import op, dst, src, imm
from dxbc_patch import container

jobs, expected = [], []


def inputs(value, ds=False):
    slots, known, identity = value[:3]
    result = dict(slots=slots, known=known, identity=identity,
                  operands=[[domain.operand(r)] + [domain.operand(r, c) for c in range(4)] for r in slots])
    if ds:
        result['domain'] = value[3]
    return result


def reference(j):
    kind = j['op']
    if kind == 'domain':
        return inputs(domain.declared(j['rows']), True)
    if kind == 'hull':
        return inputs(hull.declared(j['globals'], j['rows'], j['phase']))
    if kind == 'phases':
        return hull.layout(j['rows'])
    if kind == 'array-operand':
        return arrays.operand(j['array'], j['index'], j.get('mask', 15), j.get('swizzle'), j.get('component'), j.get('relative'))
    if kind == 'program':
        info, _, _, _, _, split, catalog = checkpoint.program(Path(j['input']).read_bytes(), j['stage'])
        return dict(profile=info['profile'], split=split, catalog=catalog)
    if kind == 'calls':
        owners, targets, labels, depth = checkpoint.subroutines(j['rows'], j['split'])
        return dict(owners=owners, targets=targets, labels=labels, depth=depth)
    if kind == 'instruction':
        prefix, ranges = checkpoint.operands_of(j['words'])
        return dict(prefix=prefix, ranges=ranges, dependent=checkpoint.dependent_result(j['words'], arrays.declarations(j['declarations'])))
    if kind == 'arrays':
        decls = arrays.declarations(j['declarations'])
        result = dict(declarations=list(decls.values()))
        if 'words' in j:
            replacements = []

            def replace(ident, offset, relative):
                index = len(replacements)
                replacements.append([ident, offset, relative])
                return src(91, index % 4)

            result['accesses'] = list(arrays.accesses(j['words'], decls))
            result['rewritten'] = arrays.rewrite_indices(j['words'], replace)
            result['replacements'] = replacements
            if j.get('destination'):
                result['destination'] = arrays.destination(j['words'], decls)
        return result
    if kind == 'changes':
        return arrays.changes_index(j['destination'], j['relative'])
    if kind == 'selector':
        return selector.validate(j['selector'], bytes(j['shader']), j['stage'], j['slots'], j['known'], j.get('phase'))
    if kind == 'snapshot':
        return selector.from_snapshot(j['result'], j['registers'], j['row'], j.get('policy', 'unique'))
    if kind == 'verify':
        selector.verify(bytes(j['data']), j['metadata'])
        return True
    raise AssertionError(kind)


def job(name, **j):
    j = copy.deepcopy(dict(name=name, **j))
    try:
        value = reference(j)
        want = dict(success=True, value=json.loads(json.dumps(value)))
    except (ValueError, KeyError, IndexError, struct.error) as error:
        want = dict(success=False, error=str(error))
    jobs.append(j)
    expected.append(want)


rng = random.Random(20260921)
decls = [op(105, [7, 8, 4]), op(105, [3, 4, 2]), op(105, [91, 4096, 1])]
for mask in range(1, 16):
    for component in range(5):
        for relative in (None, src(4, 1)):
            args = dict(op='array-operand', array=7, index=3, mask=mask)
            if component < 4:
                args['component'] = component
            if relative is not None:
                args['relative'] = relative
            job(f'operand-{len(jobs)}', **args)
for swizzle in range(256):
    job(f'operand-swizzle-{swizzle}', op='array-operand', array=3, index=0, swizzle=swizzle)
for i in range(240):
    ident = rng.choice([7, 3, 91])
    components = {7: 4, 3: 2, 91: 1}[ident]
    mask = rng.randrange(1, 1 << components)
    index = rng.randrange({7: 8, 3: 4, 91: 4096}[ident])
    relative = None if i % 3 == 0 else src(rng.randrange(16), rng.randrange(4))
    if i % 5 == 0:
        relative = arrays.operand(3, 2, component=0)
    words = arrays.operand(ident, index, mask=mask, relative=relative)
    if relative is not None and i % 2:
        words[0] = (words[0] & ~(7 << 25)) | (3 << 25)
        words.insert(2, index)
    job(f'array-destination-{i}', op='arrays', declarations=decls, words=words, destination=True)
    # Extended vector swizzles on the same address preserve extension and index order.
    words[0] = (words[0] & ~0xfff) | 6 | (rng.randrange(256) << 4) | 0x80000000
    words.insert(1, 0x41)
    job(f'array-source-{i}', op='arrays', declarations=decls, words=words)
for i, bad in enumerate([[], [105], [op(105, [7, 0, 4])], [op(105, [7, 4097, 4])],
                         [op(105, [7, 2, 0])], [op(105, [7, 2, 5])], decls + decls[:1]]):
    if bad == [105]:
        bad = [[105]]
    job(f'declaration-{i}', op='arrays', declarations=bad)
for i, words in enumerate([arrays.operand(7, 8), arrays.operand(92, 0), arrays.operand(3, 1, mask=15),
                           [0x2030f2, 7], [0x42030f2, 7, 0x10000a], [0xa2030f2, 7, 0],
                           [0x600e46, 0xffffffff, 1], [0x5002, 0x10000a, 1, 2, 3]]):
    job(f'array-edge-{i}', op='arrays', declarations=decls, words=words, destination=i == 2)
for dest in (dst(4, 1), dst(4, 2), dst(5, 1), arrays.operand(7, 2), arrays.operand(7, 0, relative=src(2, 0)), arrays.operand(3, 2)):
    for rel in (None, src(4, 0), src(4, 1), arrays.operand(7, 2, component=0), arrays.operand(7, 3, component=1), arrays.operand(7, 0, component=0, relative=src(2, 0))):
        job(f'changes-{len(jobs)}', op='changes', destination=dest, relative=rel)
for opcode in (38, 77, 78, 81, 132, 133, 142):
    for c in range(4):
        count = checkpoint.COUNTS[opcode]
        row = op(opcode, dst(4, 1), arrays.operand(7, 0, relative=src(4, c)), *[src(i) for i in range(count - 2)])
        job(f'two-result-{opcode}-{c}', op='instruction', declarations=decls, words=row)
for opcode, row in [('mov', op(54, dst(1), [0x4002, 0, 0x7fc12345, 0xffffffff, 0x80000000])),
                    ('double', op(54, dst(1), [0x5002, 0, 1, 2, 3])), ('unsupported', op(999)),
                    ('trailing', op(54, dst(1), imm(4), [4]))]:
    job('instruction-' + opcode, op='instruction', words=row, declarations=decls)

all_inputs = []
# GS identities live in the record header; temporaries and outputs never become selectors.
all_inputs.append(('gs', None, ([dict(name='v[0][2]', kind='input', vertex=0, register=2, mask=5),
                               dict(name='r0', kind='temporary', register=0, mask=15),
                               dict(name='o0', kind='output', register=0, mask=15),
                               dict(name='x3[0]', kind='indexable_temporary', array=3, element=0, mask=15)],
                              dict(primitive=dict(operand='vPrim', offset=8), gs_instance=dict(operand='vGSInstanceID', offset=12)), [])))
for d in (1, 2, 3):
    for count in (1, 3, 32):
        for mask in (1, 3, 7, 15):
            rows = [op(149 | d << 11), op(147 | count << 11), op(95, [0xb000]),
                    op(95, [(2 << 20) | (25 << 12) | 2 | mask << 4, count, 7]),
                    op(96, [(1 << 20) | (27 << 12) | 2 | mask << 4, 2, 11]),
                    op(95, [(28 << 12) | 2 | (7 if d == 2 else 3) << 4])]
            # Repeated declarations merge masks without moving the original register slot.
            rows.append(op(95, [(1 << 20) | (27 << 12) | 2 | 16, 2]))
            job(f'domain-{d}-{count}-{mask}', op='domain', rows=rows)
            all_inputs.append(('ds', None, domain.declared(rows)))
            if count == 1 and mask == 1:
                for name, mutate in [('bad-domain', lambda r: r.__setitem__(0, op(149))),
                                     ('duplicate-count', lambda r: r.append(r[1])),
                                     ('bad-register', lambda r: r[3].__setitem__(3, 32)),
                                     ('bad-control-points', lambda r: r[3].__setitem__(2, 2)),
                                     ('bad-location', lambda r: r[5].__setitem__(1, (28 << 12) | 0xf2))]:
                    bad = copy.deepcopy(rows)
                    mutate(bad)
                    job(f'domain-{d}-{name}', op='domain', rows=bad)
for phaseid, kind in enumerate(('control_points', 'fork', 'join')):
    for count in (1, 3, 32):
        globals_ = [op(147 | count << 11), op(148 | count << 11)]
        inputkind = 1 if phaseid == 0 else 25
        rows = [op(95, [0xb000]), op(95, [(22 + phaseid) << 12]),
                op(95, [(2 << 20) | inputkind << 12 | 0x32, count, 4]),
                op(95, [(2 << 20) | inputkind << 12 | 0x42, count, 4])]
        if phaseid:
            rows.append(op(97, [(2 << 20) | 26 << 12 | 0xf2, count, 2, 1]))
        if phaseid == 2:
            rows.append(op(95, [(1 << 20) | 27 << 12 | 0x12, 1]))
        phase = dict(id=phaseid, kind=kind)
        job(f'hull-{kind}-{count}', op='hull', globals=globals_, rows=rows, phase=phase)
        all_inputs.append(('hs', phase, hull.declared(globals_, rows, phase)))
        bad = copy.deepcopy(rows)
        bad[2][2] = count + 1
        job(f'hull-{kind}-{count}-range', op='hull', globals=globals_, rows=bad, phase=phase)

phases = [op(147 | 3 << 11), op(148 | 3 << 11), op(114), op(104, [2]), op(54, dst(0), imm(4)), op(62),
          op(115), op(206, [0]), op(62), op(116), op(95, [0x18000]), op(62)]
for name, rows in [('valid', phases), ('no-phase', [op(62)]), ('no-ret', phases[:-1]), ('empty', [op(114), op(104, [2])])]:
    job('phases-' + name, op='phases', rows=rows)
callrows = [op(4, [0x10a000, 10]), op(5, src(0, 1), [0x10a000, 20]), op(62),
            op(44, [0x10a000, 10]), op(4, [0x10a000, 20]), op(62), op(44, [0x10a000, 20]), op(62)]
for name, rows in [('valid', callrows), ('recursive', callrows[:-1] + [op(4, [0x10a000, 10]), op(62)]),
                   ('absent', callrows[:-2]), ('label-first', callrows[3:]),
                   ('duplicate', callrows + callrows[6:]), ('fallthrough', callrows[:2] + callrows[3:]),
                   ('relative', [op(4, [0x10a001, 10]), op(62)])]:
    job('calls-' + name, op='calls', rows=rows, split=0)

for i, (stage, phase, declared) in enumerate(all_inputs):
    slots, known = declared[:2]
    raw = b'original shader input selector identity'
    terms = [dict(name=n, component=c, bits=rng.randrange(1 << 32)) for n, c in selector.keys(slots, known)]
    value = dict(format=selector.FORMAT, shader_stage=stage, shader_sha256=hashlib.sha256(raw).hexdigest(),
                 inputs=list(reversed(terms)), match_policy='all' if i % 2 else 'unique')
    if stage == 'hs':
        value['hs_phase'] = phase
    args = dict(op='selector', selector=value, shader=list(raw), stage=stage, slots=slots, known=known, phase=phase)
    job(f'selector-{i}', **args)
    if i in (0, len(all_inputs) - 1):
        for name, mutate in [('boolean', lambda v: v['inputs'][0].__setitem__('bits', True)),
                             ('float', lambda v: v['inputs'][0].__setitem__('component', 0.0)),
                             ('negative', lambda v: v['inputs'][0].__setitem__('bits', -1)),
                             ('overflow', lambda v: v['inputs'][0].__setitem__('bits', 1 << 32)),
                             ('extra-field', lambda v: v.__setitem__('unexpected', 1)),
                             ('missing', lambda v: v['inputs'].pop()),
                             ('duplicate', lambda v: v['inputs'].__setitem__(0, v['inputs'][1])),
                             ('wrong-hash', lambda v: v.__setitem__('shader_sha256', '0' * 64)),
                             ('wrong-policy', lambda v: v.__setitem__('match_policy', 'first'))]:
            bad = copy.deepcopy(args)
            mutate(bad['selector'])
            job(f'selector-{i}-{name}', **bad)
    registers = [dict(name=r['name'], bits=[rng.randrange(1 << 32) for _ in range(4)], written=[True] * 4) for r in slots]
    meta = dict(shader_stage=stage, register_slots=slots, known_inputs=known, hs_phase=phase)
    result = dict(register_capture=meta, shader_sha256=value['shader_sha256'])
    row = dict(primitive_id=0xffffffff, gs_instance=12)
    job(f'snapshot-{i}', op='snapshot', result=result, registers=registers, row=row)
    selected = selector.from_snapshot(result, registers, row)
    stride = 32 + len(slots) * 32
    data = bytearray(stride * 3)
    for hit in range(3):
        struct.pack_into('<4I', data, hit * stride, 7, hit, row['primitive_id'], row['gs_instance'])
        for ri, r in enumerate(registers):
            struct.pack_into('<4I', data, hit * stride + 32 + ri * 16, *r['bits'])
            struct.pack_into('<4I', data, hit * stride + 32 + (len(slots) + ri) * 16, 1, 1, 1, 1)
    meta.update(input_selector=selected, record_stride=stride, registers=len(slots), matched_invocations=1)
    job(f'verify-{i}', op='verify', metadata=meta, data=list(data))
    if i == 0:
        for name, mutate in [('bits', lambda b: b.__setitem__(32, b[32] ^ 1)),
                             ('validity', lambda b: b.__setitem__(32 + len(slots) * 16, 0)),
                             ('new-invocation', lambda b: b.__setitem__(stride, 4)),
                             ('truncated', lambda b: b.pop())]:
            bad = copy.deepcopy(data)
            mutate(bad)
            job('verify-' + name, op='verify', metadata=meta, data=list(bad))
        bad = copy.deepcopy(registers)
        bad[0]['written'][0] = False
        job('snapshot-missing-input', op='snapshot', result=result, registers=bad, row=row)
        job('verify-empty', op='verify', metadata=meta, data=[])

for stage, version, rows in [('gs', 0x20040, callrows), ('gs', 0x20041, callrows), ('gs', 0x20050, callrows),
                              ('ds', 0x40050, [op(149 | 2 << 11), op(147 | 1 << 11), op(62)]),
                              ('hs', 0x30050, phases), ('gs', 0x10050, [op(62)])]:
    words = [version, 2 + sum(map(len, rows))] + [w for r in rows for w in r]
    path = a.out / f'program-{version}.dxbc'
    path.write_bytes(container({'SHDR' if version & 0xf0 == 0x40 else 'SHEX': struct.pack('<' + 'I' * len(words), *words)}))
    job(f'program-{version}', op='program', stage=stage, input=str(path))

from shaders import compile_hlsl
gs_source = '''struct V{float4 p:SV_Position;uint4 value:TEXCOORD0;};
[maxvertexcount(3)]void main(point float4 input[1]:SV_Position,uint prim:SV_PrimitiveID,inout PointStream<V> dst){
float4 a[5];[loop]for(uint i=0;i<5;i++)a[i]=float4(i+.25,prim+.5,input[0].z,42);
[loop]for(uint n=0;n<3;n++){V o;o.p=input[0];o.value=asuint(a[(prim+n)%5]);dst.Append(o);}}'''
common = 'struct V{float4 p:SV_Position;};struct C{float e[3]:SV_TessFactor;float i:SV_InsideTessFactor;};'
hs_source = common + '''C patch(InputPatch<V,3> v){C c;c.e[0]=c.e[1]=c.e[2]=c.i=2;return c;}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")][outputcontrolpoints(3)][patchconstantfunc("patch")]
V main(InputPatch<V,3> v,uint id:SV_OutputControlPointID){V o;o.p=v[id].p+1;return o;}'''
ds_source = common + '''[domain("tri")]float4 main(C c,float3 uv:SV_DomainLocation,uint prim:SV_PrimitiveID,
const OutputPatch<V,3> v):SV_Position{return v[0].p*uv.x+v[1].p*uv.y+v[2].p*uv.z+prim;}'''
for profile, source in [('gs_4_0', gs_source), ('gs_4_1', gs_source), ('gs_5_0', gs_source),
                        ('hs_5_0', hs_source), ('ds_5_0', ds_source)]:
    raw = compile_hlsl(source, profile)[0]
    path = a.out / (profile + '.dxbc')
    path.write_bytes(raw)
    stage = profile[:2]
    job('compiled-' + profile, op='program', stage=stage, input=str(path))
    _, _, _, _, rows, split, _ = checkpoint.program(raw, stage)
    if stage == 'hs':
        for phase in hull.layout(rows):
            job(f'compiled-{profile}-phase-{phase["id"]}', op='hull', globals=rows,
                rows=rows[phase['start'] + 1:phase['split']], phase=phase)
    else:
        if stage == 'ds':
            job('compiled-' + profile + '-inputs', op='domain', rows=rows[:split])
        declrows = rows[:split]
        for index, row in enumerate(rows[split:], split):
            job(f'compiled-{profile}-instruction-{index}', op='instruction', declarations=declrows, words=row)
            for start, end in checkpoint.operands_of(row)[1]:
                job(f'compiled-{profile}-operand-{index}-{start}', op='arrays', declarations=declrows, words=row[start:end])

real_count = 0
if a.real:
    from frame import Frame
    seen = set()
    for filename in ('GF2_Exilium_2026_03_03__00_19_35.gpa_frame', 'bf1_2026_01_21__16_53_05.gpa_frame'):
        with Frame(a.reference / filename) as f:
            for entry in f.entries.values():
                if entry.category != 5 or entry.type not in (0x91, 0x94, 0x95):
                    continue
                raw = f.shader(f.resource(entry.id)['data_id'])
                digest = hashlib.sha256(raw).hexdigest()
                if digest in seen:
                    continue
                seen.add(digest)
                real_count += 1
                stage = {0x91: 'gs', 0x94: 'ds', 0x95: 'hs'}[entry.type]
                name = f'real-{stage}-{digest[:12]}'
                path = a.out / (name + '.dxbc')
                path.write_bytes(raw)
                job(name, op='program', stage=stage, input=str(path))
                try:
                    _, _, _, _, rows, split, _ = checkpoint.program(raw, stage)
                except ValueError:
                    continue # Signature-only SO resources do not have an executable GS.
                if stage == 'hs':
                    for phase in hull.layout(rows):
                        job(name + '-phase-' + str(phase['id']), op='hull', globals=rows,
                            rows=rows[phase['start'] + 1:phase['split']], phase=phase)
                elif stage == 'ds':
                    job(name + '-inputs', op='domain', rows=rows[:split])
                else:
                    job(name + '-calls', op='calls', rows=rows, split=split)

manifest = a.out / 'jobs.json'
manifest.write_text(json.dumps(jobs), encoding='utf-8')
(a.out / 'expected.json').write_text(json.dumps(expected), encoding='utf-8')
env = {k: v for k, v in os.environ.items() if k.upper() in ('SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'APPDATA', 'LOCALAPPDATA')}
env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + os.environ['WINDIR'] + '/System32'
run = subprocess.run([str(a.exe.resolve()), '--probe', str(manifest)], env=env, capture_output=True, timeout=120)
(a.out / 'native.log').write_bytes(run.stdout + run.stderr)
assert run.returncode == 0, run.stderr.decode(errors='replace')
actual = json.loads(Path(str(manifest) + '.results.json').read_text())
assert len(actual) == len(jobs)
checks = []
for j, want, got in zip(jobs, expected, actual):
    # The reference only reads selected components; it can accept a record whose
    # unused final validity bytes are missing. Native records require whole strides.
    hardened = j['name'] == 'verify-truncated'
    passed = got['name'] == j['name'] and got['success'] == (False if hardened else want['success'])
    if want['success'] and not hardened:
        passed &= got.get('value') == want['value']
    checks.append(dict(name=j['name'], passed=passed, expected_rejection=not want['success'],
                       native_bounds_hardening=hardened,
                       native_error=got.get('error'), reference_error=want.get('error')))
    if not passed:
        print('FAIL', j['name'], got.get('error'), want.get('error'), flush=True)
sources = ('dxbc_indexable.py', 'dxbc_domain_inputs.py', 'dxbc_hull_phases.py', 'dxbc_gs_checkpoint.py', 'native_invocation_selector.py')
report = dict(completed=True, passed=all(c['passed'] for c in checks), checks=checks, real_shader_count=real_count,
              executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),
              reference_sources={s: hashlib.sha256((a.reference / 'standalone' / s).read_bytes()).hexdigest() for s in sources})
(a.out / 'validation.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(f"{sum(c['passed'] for c in checks)}/{len(checks)} passed; {sum(c['expected_rejection'] for c in checks)} expected rejections; {real_count} real shader resources")
sys.exit(0 if report['passed'] else 1)
