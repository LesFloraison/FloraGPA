"""Compare native Quad shader transforms with the preserved Python implementation."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--qt-bin', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path[:0] = [str(args.reference / 'standalone'), str(args.reference / 'tools')]
    import shaders
    import dxbc_patch
    import dxbc_system_id
    import quad_targets
    from frame import Frame

    jobs, wanted, labels = [], [], []
    library = {}

    def add(raw, action, label, **options):
        jobs.append(dict(input=base64.b64encode(raw).decode(), action=action, **options))
        labels.append(label)
        try:
            if action == 'offset':
                output, metadata = dxbc_system_id.offset_system_id(raw, options['offset'], options['system'])
                result = dict(metadata=metadata)
            else:
                output = quad_targets.filter_shader(raw, options['index'], options['source'])
                result = {}
            wanted.append(dict(output=base64.b64encode(output).decode(), **result))
        except (ValueError, KeyError, IndexError, struct.error):
            wanted.append(dict(error=True))

    sources = [
        'float4 main(uint id:SV_VertexID):SV_Position{return float4(id,0,0,1);}',
        'float4 main(uint id:SV_InstanceID):SV_Position{return float4(0,id,0,1);}',
        'float4 main(uint v:SV_VertexID,uint i:SV_InstanceID,uint a:ATTR):SV_Position{return uint4(v,i,a,1);}',
        'cbuffer C{float4 p[16];};float4 main(uint v:SV_VertexID,uint i:SV_InstanceID):SV_Position{return p[(v+i)%16];}',
        'float4 main(float4 p:POSITION):SV_Position{return p;}',
        'float4 main(uint v:SV_VertexID):SV_Position{double x=double(v)*1.234567890123;return float4(x,x+2,0,1);}',
    ]
    for n, source in enumerate(sources):
        for profile in (['vs_5_0'] if n == 5 else ['vs_4_0', 'vs_4_1', 'vs_5_0']):
            for optimization in ('preserve', 'optimize'):
                raw = shaders.compile_hlsl(source, profile, optimization=optimization)[0]
                library[hashlib.sha256(raw).hexdigest()] = raw
    for capture in args.reference.glob('*.gpa_frame'):
        with Frame(capture) as frame:
            for e in frame.entries.values():
                if e.category == 5 and 0x90 <= e.type <= 0x95:
                    resource = frame.resource(e.id)
                    if resource['data_id']:
                        raw = frame.shader(resource['data_id'])
                        library[hashlib.sha256(raw).hexdigest()] = raw
    for digest, raw in library.items():
        for system in (6, 8):
            for offset in (0, 1, 3, 0x80000000, 0xffffffff):
                add(raw, 'offset', digest, offset=offset, system=system)

    seed = shaders.compile_hlsl(sources[0], 'vs_5_0')[0]
    for offset in (-1, 2**32, True, 1.5):
        add(seed, 'offset', 'invalid-offset', offset=offset, system=6)
    for system in (0, 1, 7, 9):
        add(seed, 'offset', 'unsupported-system', offset=1, system=system)

    # Encoding fixtures exercise operand boundaries independently of shader validity.
    # Data resembling input tokens must remain data; relative input operands must be rewritten.
    parts = shaders.chunks(seed)
    key = 'SHEX' if 'SHEX' in parts else 'SHDR'
    header, original = dxbc_patch.instructions(parts[key])
    reg = next(s['register'] for s in shaders.inspect(seed)['signatures']['ISGN'] if s['system_value'] == 6)
    operands = [
        [0x00101e46, reg],
        [0x80101e46, 0, reg],
        [0x00208e46 | (3 << 25), 0, 2, 0x0010100a, reg],
        [0x00208e46 | (4 << 25), 0, 2, 0, 0x0010100a, reg],
        [0x4002, 0x00101e46, reg, 0xffffffff, 0],
        [0x5002, 0x00101e46, reg, 0xffffffff, 0],
        [0x0040100a, 0],  # dynamic input addressing is explicitly unsupported
        [0x0090100a, 0x0010000a, 0],
        [0x5001, 0, 0],  # unvalidated scalar double
        [0x00101e46],
        [0x80101e46],
    ]

    def program(rows):
        changed = dict(parts)
        words = header + [word for row in rows for word in row]
        words[1] = len(words)
        changed[key] = struct.pack('<' + 'I' * len(words), *words)
        return dxbc_patch.container(changed)

    declarations = [op for op in original if (op[0] & 0x7ff) in (set(range(88, 107)) | set(range(143, 163)) | {53})]
    for n, operand in enumerate(operands):
        row = [0, 0x001000f2, 0, *operand]
        row[0] = (len(row) << 24) | 54
        add(program([*declarations, row, [0x0100003e]]), 'offset', f'operand-{n}', offset=7, system=6)
    for depth in (2, 16, 32, 33, 34):
        operand = [0x0010100a, reg]
        for _ in range(depth):
            operand = [0x00208e46 | (2 << 25), 0, *operand]
        row = [0, 0x00100012, 0, *operand]
        row[0] = (len(row) << 24) | 54
        add(program([*declarations, row, [0x0100003e]]), 'offset', f'recursion-{depth}', offset=7, system=6)
    extended = [0x86000036, 0, 0x00100012, 0, 0x0010100a, reg]
    interface = [0x04000078, 3, 0x00113000, 0]
    for row in (extended, interface):
        add(program([*declarations, row, [0x0100003e]]), 'offset', 'instruction-prefix', offset=7, system=6)
    for count in (0, 4095, 4096):
        rows = [r for r in declarations if r[0] & 0x7ff != 104]
        rows += [[0x02000068, count], [0x05000036, 0x00100012, 0, 0x0010100a, reg], [0x0100003e]]
        add(program(rows), 'offset', f'temps-{count}', offset=2, system=6)
    for rows in ([*declarations, [0x010007ff]], [*declarations, [0x0200003e, 0]],
                 [*declarations, [0x02000068, 1], [0x02000068, 2], [0x0100003e]]):
        add(program(rows), 'offset', 'malformed-instruction', offset=1, system=6)

    counter_source = (args.reference / 'standalone/quad_counter.hlsl').read_text()
    import re
    counter_source = re.sub(r'register\(u(\d+)\)', lambda m: f'register(u{int(m[1])+1})', counter_source)
    counter = shaders.compile_hlsl(counter_source, 'ps_5_0', entry='quadOverdrawCounterPS')[0]
    references = [counter,
        shaders.compile_hlsl('RWTexture1D<uint> hits:register(u4);[earlydepthstencil]void main(float4 p:SV_Position){InterlockedAdd(hits[0],1);}', 'ps_5_0')[0],
        shaders.compile_hlsl('RWBuffer<uint> hits:register(u4);void main(uint layer:SV_RenderTargetArrayIndex){InterlockedAdd(hits[layer],1);}', 'ps_5_0')[0]]
    for raw in references:
        for index in (None, 0, 1, 2048, 0xffffffff):
            sources = [None] + [dict(register=r, mask=m, component_type=1) for r in (0, 1, 2, 7, 31) for m in (1, 2, 4, 8)]
            sources += [dict(register=32, mask=1, component_type=1), dict(register=3, mask=3, component_type=1), dict(register=3, mask=1, component_type=3)]
            for source in sources:
                add(raw, 'filter', 'counter-array', index=index, source=source)

    input_path, output_path = args.out / 'jobs.json', args.out / 'actual.json'
    input_path.write_text(json.dumps(jobs))
    env = dict(os.environ)
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + str(Path(os.environ['SystemRoot']) / 'System32')
    run = subprocess.run([str(args.probe.resolve()), '--probe', str(input_path.resolve()), str(output_path.resolve())],
                         env=env, capture_output=True, timeout=120)
    (args.out / 'probe.log').write_bytes(run.stdout + run.stderr)
    assert run.returncode == 0, run.stderr
    actual = json.loads(output_path.read_text())
    assert len(actual) == len(wanted)
    failures = []
    for i, (expected, result) in enumerate(zip(wanted, actual)):
        if bool('error' in expected) != bool('error' in result) or ('error' not in expected and expected != result):
            failures.append(dict(index=i, label=labels[i], expected=expected, actual=result))
    report = dict(passed=not failures, comparisons=len(jobs), shaders=len(library),
                  expected_rejections=sum('error' in r for r in wanted), failures=failures,
                  probe_sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest())
    (args.out / 'validation.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v for k, v in report.items() if k != 'failures'}))
    assert not failures, [(f['index'], f['label']) for f in failures[:10]]


if __name__ == '__main__':
    main()
