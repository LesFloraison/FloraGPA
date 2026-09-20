"""Development oracle: compare native DXBC output-log bytes and metadata with Python.

This validates instrumentation, not replay isolation, exports, or UI integration.
The probe is a test executable; Python is never an application dependency.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--qt-bin', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--real', action='store_true')
a = p.parse_args()
a.out = a.out.resolve()
a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference / 'standalone'), str(a.reference / 'tools')]
from shaders import compile_hlsl, chunks, inspect
from dxbc_patch import instructions, container
from dxbc_vertex_writes import instrument as writes
from dxbc_geometry_emissions import instrument as emissions
from dxbc_vertex_identity import instrument as identity
from so_counts import topologies
from frame import Frame

jobs, expected = [], []


def add(name, raw, stage, slot=63, capacity=19, stream=0, reject=False, instances=3):
    source = a.out / (name + '.input.dxbc')
    target = a.out / (name + '.native.dxbc')
    source.write_bytes(raw)
    job = dict(name=name, input=str(source), output=str(target), stage=stage,
               slot=slot, capacity=capacity, stream=stream, instances=instances)
    error = None
    try:
        if stage == 'gs':
            patched, metadata = emissions(raw, slot, capacity, stream)
        elif stage == 'identity':
            patched, metadata = identity(raw, instances)
        else:
            patched, metadata = writes(raw, slot, capacity)
    except (ValueError, KeyError, IndexError, struct.error) as ex:
        error = str(ex)
    if bool(error) != reject:
        raise AssertionError((name, 'unexpected oracle outcome', error, reject))
    record = dict(name=name, expected_rejection=reject, reference_error=error)
    if not error:
        (a.out / (name + '.python.dxbc')).write_bytes(patched)
        record.update(metadata=metadata, sha256=hashlib.sha256(patched).hexdigest())
    jobs.append(job)
    expected.append(record)


def rewrite(raw, edit):
    parts = chunks(raw)
    key = 'SHEX' if 'SHEX' in parts else 'SHDR'
    header, rows = instructions(parts[key])
    edit(rows)
    words = header + [v for row in rows for v in row]
    words[1] = len(words)
    parts[key] = struct.pack('<' + str(len(words)) + 'I', *words)
    return container(parts)


vs_sources = {
    'ids': 'float4 main(uint id:SV_VertexID,uint inst:SV_InstanceID):SV_Position{return float4(id,inst,0,1);}',
    'no-ids': 'float4 main(float4 p:POSITION):SV_Position{return p;}',
    'typed': 'struct V{float4 p:SV_Position;uint2 id:ID;int2 si:SIGNED;float2 f:VALUE;};V main(uint id:SV_VertexID,uint inst:SV_InstanceID){V o;o.p=float4(id,inst,0,1);o.id=uint2(id,inst);o.si=int2(-int(id),-2);o.f=float2(id*.5,inst+.25);return o;}',
    'flow': 'float4 main(uint id:SV_VertexID):SV_Position{float x=id;[loop]for(uint i=0;i<id;i++){if(i==2)continue;x+=i;if(x>99)break;}if(id==4)return 1;return float4(x,0,0,1);}',
    'wide': 'struct V{float4 p:SV_Position;' + ''.join(f'float4 a{i}:ATTR{i};' for i in range(31)) + '};V main(uint id:SV_VertexID,uint inst:SV_InstanceID){V o;o.p=float4(id,inst,0,1);' + ''.join(f'o.a{i}=float4(id+{i},inst,{i},1);' for i in range(31)) + 'return o;}',
}
vs_codes = {}
for profile in ('vs_4_0', 'vs_4_1', 'vs_5_0'):
    for name, source in vs_sources.items():
        if name == 'wide' and profile.startswith('vs_4'):
            for i in range(15, 31):
                source = source.replace(f'float4 a{i}:ATTR{i};', '').replace(f'o.a{i}=float4(id+{i},inst,{i},1);', '')
        raw = compile_hlsl(source, profile)[0]
        vs_codes[profile, name] = raw
        for slot in (0, 7, 8, 63):
            for capacity in (1, 1024):
                add(f'{profile}-{name}-{slot}-{capacity}', raw, 'vs', slot, capacity)
        if name != 'wide':
            for count in (1, 3):
                add(f'identity-{profile}-{name}-{count}', raw, 'identity', instances=count)

for domain in ('tri', 'quad', 'isoline'):
    from validate_domain_writes import fixture as domain_fixture
    for known in (True, False):
        path, source = domain_fixture(a.out / f'ds-{domain}-{known}.gpa_frame', domain=domain, known=known)
        raw = compile_hlsl(source, 'ds_5_0')[0]
        for slot in (0, 7, 8, 63):
            for capacity in (1, 1024):
                add(f'ds-{domain}-{known}-{slot}-{capacity}', raw, 'ds', slot, capacity)

from validate_geometry_emissions import fixture as gs_fixture
gs_codes = {}
for profile in ('gs_4_0', 'gs_4_1', 'gs_5_0'):
    for topology in ('point', 'line', 'triangle'):
        path, source = gs_fixture(a.out / f'{profile}-{topology}.gpa_frame', profile=profile, topology=topology)
        raw = compile_hlsl(source, profile)[0]
        gs_codes[profile, topology] = raw
        for slot in (0, 7, 8, 63):
            for capacity in (1, 1024):
                add(f'{profile}-{topology}-{slot}-{capacity}', raw, 'gs', slot, capacity)
for name, options in (('wide', dict(wide=True)), ('early-return', dict(zero=True))):
    path, source = gs_fixture(a.out / f'gs-{name}.gpa_frame', **options)
    add('gs-' + name, compile_hlsl(source, 'gs_5_0')[0], 'gs')

from validate_so_passthrough import multi_fixture, signature
path = a.out / 'multi.gpa_frame'
multi_fixture(path, 2)
with Frame(path) as frame:
    multi = frame.shader(13)
for stream in range(4):
    add(f'gs-stream-{stream}', multi, 'gs', stream=stream)


def cross_stream(rows):
    at = next(i for i, row in enumerate(rows) if row[0] & 2047 == 117)
    assert rows[at][2] == 0 and rows[at + 3][2] == 1
    del rows[at + 1:at + 3]


cross = rewrite(multi, cross_stream)
for stream in range(4):
    add(f'gs-cross-stream-{stream}', cross, 'gs', stream=stream)


def combine(rows):
    i, changed = 0, 0
    while i + 1 < len(rows):
        opcode = rows[i][0] & 2047
        if opcode in (19, 117) and rows[i + 1][0] & 2047 in (9, 118):
            rows[i][0] = (rows[i][0] & ~2047) | (20 if opcode == 19 else 119)
            del rows[i + 1]
            changed += 1
        i += 1
    assert changed


combined_source = 'struct V{float4 p:SV_Position;};[maxvertexcount(1)]void main(point V input[1],inout PointStream<V> s){s.Append(input[0]);s.RestartStrip();}'
for profile in ('gs_4_0', 'gs_4_1', 'gs_5_0'):
    add(profile + '-combined', rewrite(compile_hlsl(combined_source, profile)[0], combine), 'gs')

simple = compile_hlsl('float4 main(uint id:SV_VertexID):SV_Position{return float4(id,1,2,3);}', 'vs_5_0')[0]


def sparse(rows):
    changed = 0
    for row in rows:
        if row[0] & 2047 == 54 and (row[1] >> 12) & 255 == 2 and (row[1] >> 4) & 15 == 14:
            row[1] = (row[1] & ~240) | 32
            changed += 1
    assert changed == 1


add('vs-sparse', rewrite(simple, sparse), 'vs')
for profile in ('vs_5_0', 'ds_5_0', 'gs_5_0'):
    from validate_pre_raster_uav import fixture as uav_fixture
    path = a.out / (profile + '-uav.gpa_frame')
    uav_fixture(path, profile[:2], 7)
    with Frame(path) as frame:
        candidates = [e for e in frame.entries.values() if e.category == 5 and e.type == {'vs_5_0': 0x90, 'ds_5_0': 0x94, 'gs_5_0': 0x91}[profile]]
        raw = frame.shader(frame.resource(candidates[0].id)['data_id'])
    add(profile + '-existing-uav', raw, profile[:2])
    add(profile + '-collision', raw, profile[:2], slot=7, reject=True)

for name, slot, capacity in (('slot', 64, 1), ('zero', 63, 0), ('bytes', 63, 0xffffffff)):
    for stage, raw in (('vs', simple), ('gs', gs_codes['gs_5_0', 'point'])):
        add(stage + '-invalid-' + name, raw, stage, slot, capacity, reject=True)
add('gs-undeclared-stream', gs_codes['gs_5_0', 'point'], 'gs', stream=1, reject=True)
add('gs-signature-only', signature(gs_codes['gs_5_0', 'point']), 'gs', reject=True)
add('vs-wrong-stage', gs_codes['gs_5_0', 'point'], 'vs', reject=True)
add('gs-wrong-stage', simple, 'gs', reject=True)
for n in (0, 3, 32, len(simple) - 1):
    add(f'truncated-{n}', simple[:n], 'vs', reject=True)

if a.real:
    seen = set()
    for label, filename in (('gf2', 'GF2_Exilium_2026_03_03__00_19_35.gpa_frame'), ('bf1', 'bf1_2026_01_21__16_53_05.gpa_frame')):
        with Frame(a.reference / filename) as frame:
            for e in frame.entries.values():
                if e.category != 5 or e.type not in (0x90, 0x91, 0x94):
                    continue
                raw = frame.shader(frame.resource(e.id)['data_id'])
                digest = hashlib.sha256(raw).hexdigest()
                if digest in seen:
                    continue
                seen.add(digest)
                info = inspect(raw, signature_only=True)
                stage = info['stage']
                if stage not in ('vs', 'ds', 'gs'):
                    continue
                streams = sorted(topologies(raw)) if stage == 'gs' else [0]
                for stream in streams:
                    add(f'real-{label}-{e.id}-{stage}-{stream}', raw, stage, stream=stream)

manifest = a.out / 'jobs.json'
manifest.write_text(json.dumps(jobs, indent=2))
(a.out / 'reference.json').write_text(json.dumps(expected, indent=2))
env = {k: v for k, v in os.environ.items() if k.upper() in ('SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'APPDATA', 'LOCALAPPDATA')}
env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + os.environ['WINDIR'] + '/System32;' + os.environ['WINDIR']
run = subprocess.run([str(a.exe.resolve()), '--probe', str(manifest)], env=env, capture_output=True, timeout=300)
(a.out / 'native.log').write_bytes(run.stdout + run.stderr)
run.check_returncode()
actual = json.loads(Path(str(manifest) + '.results.json').read_text())
assert len(actual) == len(expected)
checks = []
for job, want, got in zip(jobs, expected, actual):
    differences = []
    if got['name'] != want['name'] or got['success'] == want['expected_rejection']:
        differences.append(dict(outcome=got))
    elif got['success']:
        if got['metadata'] != want['metadata']:
            differences.append(dict(metadata=dict(expected=want['metadata'], actual=got['metadata'])))
        digest = hashlib.sha256(Path(job['output']).read_bytes()).hexdigest()
        if digest != want['sha256']:
            differences.append(dict(bytes=dict(expected=want['sha256'], actual=digest)))
    checks.append(dict(name=job['name'], passed=not differences, expected_rejection=want['expected_rejection'], differences=differences))
sources = ('dxbc_vertex_writes.py', 'dxbc_geometry_emissions.py', 'dxbc_vertex_identity.py', 'dxbc_hull_instance.py', 'dxbc_hull_capture.py', 'dxbc_operand_arities.json', 'dxbc_uav.py', 'dxbc_immediate.py')
result = dict(completed=True, passed=all(c['passed'] for c in checks), checks=checks,
              executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),
              reference_sources={s: hashlib.sha256((a.reference / 'standalone' / s).read_bytes()).hexdigest() for s in sources})
(a.out / 'validation.json').write_text(json.dumps(result, indent=2))
for c in checks:
    if not c['passed']:
        print(c['name'], 'FAIL', c['differences'])
print(f"{sum(c['passed'] for c in checks)}/{len(checks)} passed; {sum(c['expected_rejection'] for c in checks)} expected rejections", flush=True)
sys.exit(0 if result['passed'] else 1)
