"""Development-only comparison of native external shader tools with the Python original."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--tool', type=Path, required=True)
    parser.add_argument('--qt-bin', type=Path, required=True)
    parser.add_argument('--shader-dir', type=Path, required=True,
                        help='Directory containing cli-{vs,ps,gs,hs,ds,cs}/shader.dxbc exports')
    parser.add_argument('--cli', type=Path)
    parser.add_argument('--capture', type=Path, help='BF1 fixture containing PS resource 229')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.reference.resolve()))
    import shaders
    env = dict(os.environ)
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + str(Path(os.environ['SystemRoot'])/'System32')
    rows = []

    def native(name, action, code, **kw):
        job = dict(action=action, bytecode=code.hex(), out=str(args.out/(name+'-native')),
                   tool=str(args.tool.resolve()), **kw)
        path = args.out/(name+'-job.json')
        output = args.out/(name+'-native.json')
        path.write_text(json.dumps(job), encoding='utf-8')
        run = subprocess.run([str(args.probe.resolve()), '--probe', str(path), str(output)],
                             env=env, capture_output=True, timeout=90)
        report = json.loads(output.read_text(encoding='utf-8'))
        if run.returncode and 'error' not in report:
            raise RuntimeError(report)
        return Path(job['out']), report

    for stage in ['vs', 'ps', 'gs', 'hs', 'ds', 'cs']:
        code = (args.shader_dir/f'cli-{stage}'/'shader.dxbc').read_bytes()
        source = shaders.disassemble(code)
        source_file = args.out/(stage+'.asm')
        source_file.write_text(source, encoding='utf-8')
        expected_dir = args.out/(stage+'-reference')
        expected_dir.mkdir()
        expected = shaders.assemble(source_file, code, expected_dir, args.tool)
        actual_dir, actual = native(stage, 'assemble', code,
                                    source=source_file.read_bytes().decode('utf-8'))
        checks = {key: expected[key] == actual[key] for key in
                  ['tool', 'tool_sha256', 'original_sha256', 'replacement_sha256', 'stage', 'profile']}
        for file in ['edited.asm', 'original.dxbc', 'edited.shdr', 'replacement.dxbc']:
            checks[file] = (expected_dir/file).read_bytes() == (actual_dir/file).read_bytes()
        rows.append(dict(name=stage, checks=checks, passed=all(checks.values())))

    for name, source, profile in [
        ('texture1d', 'Texture1D<float4> data:register(t0);SamplerState samp:register(s0);'
         'float4 main(float x:TEXCOORD):SV_Target{return data.Sample(samp,x);}', 'ps_5_0'),
        ('texture1darray', 'Texture1DArray<float4> data:register(t0);SamplerState samp:register(s0);'
         'float4 main(float2 x:TEXCOORD):SV_Target{return data.Sample(samp,x);}', 'ps_5_0'),
        ('native-first', 'float4 main():SV_Target{return 1;}', 'ps_5_0'),
        ('consume', 'ConsumeStructuredBuffer<float4> data:register(u0);'
         'RWStructuredBuffer<float4> target:register(u1);'
         '[numthreads(1,1,1)]void main(){target[0]=data.Consume();}', 'cs_5_0'),
    ]:
        code, _ = shaders.compile_hlsl(source, profile)
        expected_dir = args.out/(name+'-reference')
        expected_dir.mkdir()
        try:
            expected = shaders.decompile(code, expected_dir, args.tool)
        except (ValueError, RuntimeError) as exc:
            expected = {'error': str(exc)}
        actual_dir, actual = native(name, 'recover', code)
        (args.out/(name+'-reference.json')).write_text(json.dumps(expected, indent=2), encoding='utf-8')
        checks = {'rejected': 'error' in expected and 'error' in actual} if 'error' in expected else {'report': expected == actual}
        for file in ['reconstructed.dxbc', 'reconstructed.hlsl', 'reconstructed.recompiled.dxbc']:
            left, right = expected_dir/file, actual_dir/file
            checks[file] = left.exists() == right.exists() and (not left.exists() or left.read_bytes() == right.read_bytes())
        rows.append(dict(name=name, checks=checks, passed=all(checks.values())))

    if args.cli:
        if not args.capture:
            parser.error('--cli requires --capture')
        for profile in ['ps_4_0', 'ps_4_1', 'ps_5_0']:
            for optimization in ['auto', 'preserve', 'optimize']:
                name = profile+'-'+optimization
                source = '// FloraGPA compiler optimization: preserve\nfloat4 custom():SV_Target{return float4(0,1,0,1);}'
                path = args.out/(name+'.hlsl')
                path.write_bytes(source.encode('utf-8'))
                expected, diagnostics = shaders.compile_hlsl(source, profile, 'custom', str(path), optimization)
                out = args.out/(name+'-cli')
                run = subprocess.run([str(args.cli.resolve()), 'compile', str(args.capture.resolve()),
                    '--id', '229', '--source', str(path), '--profile', profile, '--entry', 'custom',
                    '--optimization', optimization, '--out', str(out)], env=env, capture_output=True, timeout=90)
                report = json.loads((out/'report.json').read_text(encoding='utf-8'))
                checks = {'completed': run.returncode == 0 and report['completed'],
                          'diagnostics': report['diagnostics'] == diagnostics,
                          'compilation': report['compilation'] == shaders.compilation_options(source, optimization),
                          'bytecode': (out/'replacement.dxbc').read_bytes() == expected,
                          'source': (out/'replacement.hlsl').read_bytes() == path.read_bytes(),
                          'assembly': (out/'replacement.asm').read_bytes() == shaders.disassemble(expected).replace('\n', '\r\n').encode('utf-8')}
                rows.append(dict(name=name, checks=checks, passed=all(checks.values())))

        export = args.out/'shader-cli'
        run = subprocess.run([str(args.cli.resolve()), 'shader', str(args.capture.resolve()),
            '--id', '229', '--out', str(export)], env=env, capture_output=True, timeout=90)
        if run.returncode:
            raise RuntimeError(run.stderr.decode('utf-8', errors='replace'))
        assembly = export/'shader.asm'
        checks = {'assembly': assembly.read_bytes() == (args.out/'ps-reference'/'edited.asm').read_bytes()}
        rows.append(dict(name='shader-cli', checks=checks, passed=all(checks.values())))
        out = args.out/'assemble-cli'
        run = subprocess.run([str(args.cli.resolve()), 'assemble', str(args.capture.resolve()),
            '--id', '229', '--source', str(assembly), '--decompiler', str(args.tool.resolve()),
            '--out', str(out)], env=env, capture_output=True, timeout=90)
        if run.returncode:
            raise RuntimeError(run.stderr.decode('utf-8', errors='replace'))
        report = json.loads((out/'report.json').read_text(encoding='utf-8'))
        checks = {'completed': run.returncode == 0 and report['completed'],
                  'bytecode': (out/'replacement.dxbc').read_bytes() == (args.out/'ps-reference'/'replacement.dxbc').read_bytes()}
        rows.append(dict(name='assemble-cli', checks=checks, passed=all(checks.values())))

    report = dict(passed=all(row['passed'] for row in rows), cases=rows,
                  tool_sha256=hashlib.sha256(args.tool.read_bytes()).hexdigest())
    (args.out/'validation.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
