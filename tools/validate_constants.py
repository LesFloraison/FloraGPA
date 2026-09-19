"""Development-only constant reflection, packing and event inspection parity checks."""
import argparse
import base64
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--captures', type=Path, required=True)
p.add_argument('--qt-bin', type=Path, required=True)
p.add_argument('--fixture', type=Path, required=True, help='Evidence emitted by FloraConstantTests')
p.add_argument('--out', type=Path, required=True)
args = p.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
sys.path.insert(0, str(args.reference.resolve()))
import constants
import shaders
from frame import Frame

results = []
raw = (args.fixture / 'buffer.bin').read_bytes()
reflected = shaders.inspect((args.fixture / 'shader.dxbc').read_bytes())['constant_buffers'][0]['variables']
for item, expected in zip(json.loads((args.fixture / 'constants.json').read_text()), reflected, strict=True):
    variable = item['variable']
    assert variable == {k: expected[k] for k in variable}, variable['name']
    fields = constants.fields(expected, raw, 2)
    assert item['fields'] == fields, variable['name']
    for edit, field in zip(item['edits'], fields, strict=True):
        assert constants.patches(field, field['value']) == [], field['path']
        patches = [dict(offset=offset, hex=data.hex()) for offset, data in constants.patches(field, edit['value'])]
        assert patches == edit['patches'], field['path']
    results.append(dict(case='synthetic-' + variable['name'], fields=len(fields), passed=True))
print('Recursive compiled reflection, values and patches PASS', flush=True)

env = dict(os.environ)
env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env['PATH']
def run(command, log):
    result = subprocess.run(command, env=env, capture_output=True, timeout=180)
    log.write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f'{log}: {result.stderr.decode(errors="replace")[-2000:]}')

def asset(data):
    return dict(data=base64.b64encode(data).decode(), sha256=hashlib.sha256(data).hexdigest())

gf2 = args.captures / 'GF2_Exilium_2026_03_03__00_19_35.gpa_frame'
bf1 = args.captures / 'bf1_2026_01_21__16_53_05.gpa_frame'
fields_source = '''struct Light { float3 tint; bool active; };
cbuffer Fields:register(b0) {
 Light lights[2]; row_major float2x3 basis; column_major float3x2 camera; float4 special;
};
'''
pixel_source = fields_source + 'float4 main():SV_Target{return float4(lights[1].tint + basis[0] + camera[0][0],special.w);}'
compute_source = fields_source + 'RWByteAddressBuffer output:register(u0); [numthreads(1,1,1)] void main(){output.Store(0,asuint(lights[0].tint.x + basis[1][2] + camera[2][1] + special.x));}'
for key, capture, event, resource, shader, source in [
    ('gf2', gf2, 113, 104, 102, pixel_source),
    ('bf1-range', bf1, 209, 35, 190, compute_source),
]:
    source_path = args.out / f'{key}.hlsl'
    source_path.write_text(source, encoding='utf-8')
    compiled = args.out / (key + '-compiled')
    run([str(args.exe.resolve()), 'compile', str(capture), '--id', str(shader), '--source', str(source_path), '--out', str(compiled)],
        args.out / (key + '-compile.log'))
    with Frame(capture) as frame:
        digest = hashlib.sha256(frame.bytes).hexdigest()
    project = dict(format='FloraGPA experiment 1', frame_sha256=digest, frame_name=capture.name, cursor=1,
                   history=[dict(label='Reflected shader', operations=[dict(kind='shader', resource=shader,
                            asset=asset((compiled / 'replacement.dxbc').read_bytes()))])])
    path = args.out / (key + '-project.json')
    for variant in ('original', 'reflected', 'edited', 'bits', 'after', 'undo'):
        project['cursor'] = {'original':0, 'reflected':1, 'edited':2, 'bits':3, 'after':3, 'undo':1}[variant]
        path.write_text(json.dumps(project), encoding='utf-8')
        native, reference = (args.out / f'{key}-{variant}-{label}' for label in ('native', 'reference'))
        # A small byte export must still reflect the entire buffer at its original offsets.
        shared = ['--id', str(resource), '--event', str(event), '--offset', '3', '--length', '17', '--experiment', str(path)]
        run([str(args.exe.resolve()), 'buffer', str(capture), '--out', str(native), *shared,
             *([] if variant == 'after' else ['--before'])], args.out / f'{key}-{variant}-native.log')
        run([sys.executable, str(args.reference / 'analyze.py'), str(capture), 'buffer', '--out', str(reference), *shared,
             *(['--after'] if variant == 'after' else [])], args.out / f'{key}-{variant}-reference.log')
        actual = json.loads((native / 'report.json').read_text())['constant_bindings']
        expected = json.loads((reference / 'result.json').read_text())['constant_bindings']
        assert actual == expected, (key, variant, 'constant bindings differ')
        for binding in actual:
            for variable in binding['variables']:
                for field in variable['fields']:
                    if field['status'] == 'ready':
                        assert constants.patches(field, field['value']) == [], (key, variant, field['path'], 'round-trip changed bits')
        assert (native / 'buffer.bin').read_bytes() == (reference / 'buffer.bin').read_bytes()
        if variant == 'reflected':
            baseline = actual
            target = next(b for b in actual if b['name'] == 'Fields')
            if key == 'bf1-range':
                assert target['first_constant'] > 0 and target['constant_count'] is not None, target
            available = [f for v in target['variables'] for f in v['fields'] if f['status'] == 'ready']
            assert available, (key, 'no reflected ready fields')
            operations = []
            for field in available:
                cls, rows, cols = field['class_id'], field['rows'], field['columns']
                value = False if field['base_type'] == 1 else 13
                if cls in (2,3): value = [[value] * cols for _ in range(rows)]
                elif cls == 1: value = [value] * cols
                for offset, data in constants.patches(field, value):
                    operations.append(dict(kind='buffer', event=event, resource=resource, offset=offset, asset=asset(data)))
            assert operations
            project['history'].append(dict(label='Constant fields', operations=operations))
        elif variant == 'edited':
            assert actual != baseline, (key, 'edit ineffective')
            special = next(f for b in actual for v in b['variables'] for f in v['fields'] if f['path'] == 'special')
            project['history'].append(dict(label='Exact bits', operations=[dict(kind='buffer', event=event,
                resource=resource, offset=special['buffer_offset'],
                asset=asset(bytes.fromhex('3412c07f000000800000807f000080ff')))]))
        elif variant == 'bits':
            special = next(f for b in actual for v in b['variables'] for f in v['fields'] if f['path'] == 'special')
            assert special['value'] == ['nan', -0.0, 'inf', '-inf']
            assert math.copysign(1, special['value'][1]) < 0, 'CLI lost negative zero'
        elif variant in ('after', 'undo'):
            assert actual == baseline, (key, 'input restoration failed')
        results.append(dict(case=key, variant=variant, passed=True,
                            fields=sum(len(v['fields']) for b in actual for v in b['variables'])))
        print(key, variant, 'PASS', flush=True)
(args.out / 'validation.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
