"""Compare native virtual HLSL projects with the preserved Python implementation."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--qt-bin', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cli', type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.reference / 'standalone'))
    import shader_project as original
    import shader_sources
    import shaders
    jobs = []

    def add(action, value, **options):
        jobs.append(dict(action=action, value=value, **options))

    paths = ['', '.', '..', 'a/../', 'a/../../b', '/a/../../b', '\\a',
             'C:', 'C:foo', 'C:/a/../b', '//host/share', '//host/share/../x',
             '//?/UNC/host/share/a/../x', '//?/C:/a/..', '//./device/a',
             '///a', '////a/b', 'a\x00b', 'a\nb', 'Straße/İΣςﬀ.hlsl',
             '😀:/x', '😀:relative', '😀' * 32767, '😀' * 32768, None, 42]
    for p in paths:
        add('path', p)
    rng = random.Random(8291)
    for _ in range(1800):
        prefix = rng.choice(['', '/', '\\', 'C:/', 'C:', '//host/share/',
                             '//?/UNC/server/share/', '//?/C:/', '////'])
        segments = [rng.choice(['a', 'B', '..', '.', '', 'Straße', '中文', '😀', 'ς'])
                    for _ in range(rng.randrange(1, 8))]
        add('path', prefix + rng.choice(['/', '\\']).join(segments))
    macros = ['', ' /DX /DY=1 ', '/DX=two words /DY="with /DX inside"',
              "/DX='a\\'b' /DE=", '/DX="bad', '/D1A', '/DA-b', '/DA= /DB=',
              '/DA=foo\u2003/DB=bar', '/DA\x1c/DB', '/DA=1 /DA=2', '/DAé',
              '/DA=unquoted/path /DB=last', '/DA=1 /D9bad', '-DA=1']
    for m in macros:
        add('defines', m)
    for _ in range(120):
        s = rng.choice([' ', '\t', '\u2003', '\x85']).join(
            '/D' + rng.choice(['A', '_B2', 'Hello']) + rng.choice(
                ['', '=0', '="hello world"', "='quote /DA'", '=a\\b', '='])
            for _ in range(rng.randrange(1, 8)))
        add('defines', s)
    base = dict(format=original.FORMAT, root='root/main.hlsl', entry='main', profile='ps_5_0',
                files=[dict(name='root/main.hlsl', text='#include "value.hlsl"\nfloat4 main():SV_Target{return VALUE;}'),
                       dict(name='root/value.hlsl', text='#define VALUE 0.25')])
    add('validate', base)
    for key, values in {
        'flags': [False, -1, 0, 2048, 2**32-1, 2**32, 2**64-1, 1.5, None, '2048'],
        'root': ['', 'ROOT/./MAIN.HLSL', None, 'C:foo', 'missing'],
        'entry': ['', '0main', 'é', 'main_2', None],
        'profile': ['vs_4_0', 'ds_4_1', 'cs_5_0', 'ps_6_0', None],
        'unresolved_defines': ['', False, None, 0, -0.0, [], {}, 'bad', True, [0]],
        'include_dirs': [[], ['.'], ['C:foo'], [None], '.', ['.']*257],
        'defines': [None, {}, [], [{'name': 'A', 'value': '1'}, {'name': 'A', 'value': '2'}],
                    [{'name': 'A', 'value': '1'}, {'name': 'a', 'value': '2'}],
                    [{'name': 'A', 'value': '😀'*65536}], [{'name': 'A', 'value': '😀'*65537}],
                    [{'name': 'A', 'value': 'x\0y'}], [{'name': 'A', 'value': 1}]],
        'files': [[], None, [{'name': 'x', 'text': '\0'}],
                  base['files'] + [{'name': 'ROOT\\VALUE.HLSL', 'text': ''}],
                  [{'name': 'root/main.hlsl', 'text': 'é'*(8*1024*1024)}],
                  [{'name': 'root/main.hlsl', 'text': 'é'*(8*1024*1024+1)}]],
        'unknown': [True],
    }.items():
        for value in values:
            p = copy.deepcopy(base)
            p[key] = value
            add('validate', p)
    for flags in ['0x800', '2048', '0b100', '0o100', '0x_800', '2_048', '010', '0', '-1', ' +0x800 ']:
        report = dict(files=[dict(name='中文.hlsl', text='// source', text_valid=True)],
                      environment=dict(hlslFlags=flags, hlslDefines='/DX=1 /DY="two words"'))
        add('sources', report, profile='ps_5_0')
    for raw in ['', '/DX="unterminated', '/DX=1']:
        report = dict(files=[dict(name='a.hlsl', text='', text_valid=True),
                             dict(name='b.hlsl', text='', text_valid=True)],
                      environment=dict(hlslDefines=raw))
        add('sources', report, profile='vs_5_0')
    for flags in [0, 1, 5, 2048, 2304]:
        p = copy.deepcopy(base)
        p['flags'] = flags
        add('compile', p)
    for system in [False, True]:
        p = copy.deepcopy(base)
        p['include_dirs'] = ['shared']
        p['files'].append(dict(name='shared/value.hlsl', text='#define VALUE 0.75'))
        if system:
            p['files'][0]['text'] = p['files'][0]['text'].replace('"value.hlsl"', '<value.hlsl>')
        add('compile', p)
    p = copy.deepcopy(base)
    p['files'][1]['text'] = '#include "nested/inner.hlsl"\n'
    p['files'] += [dict(name='root/nested/inner.hlsl', text='#include "../../empty.hlsl"\n#define VALUE 0.5'),
                   dict(name='empty.hlsl', text='')]
    add('compile', p)
    for name in ['C:/root/main.hlsl', '//host/share/main.hlsl', '中文/main.hlsl']:
        p = copy.deepcopy(base)
        p['root'] = p['files'][0]['name'] = name
        p['files'][1]['name'] = 'value.hlsl'
        p['include_dirs'] = ['.']
        add('compile', p)
    p = copy.deepcopy(base)
    p['defines'] = [dict(name='OVERRIDE', value='0.625')]
    p['files'][1]['text'] = '#define VALUE OVERRIDE'
    add('compile', p)
    sys.path.insert(0, str(args.reference / 'tools'))
    from validate_class_linkage import HULL, DOMAIN
    bodies = {
        'vs': 'float4 main(uint i:SV_VertexID):SV_Position{return adjust(float4(i,0,0,1));}',
        'ps': 'float4 main():SV_Target{return adjust(1);}',
        'cs': 'RWBuffer<float4> result:register(u0);[numthreads(1,1,1)]void main(){result[0]=adjust(1);}',
        'gs': 'struct V{float4 p:SV_Position;};[maxvertexcount(1)]void main(point V input[1],inout PointStream<V> output){V v;v.p=adjust(input[0].p);output.Append(v);}',
        'hs': HULL.replace('EXPR', 'adjust(input[id].p)'),
        'ds': DOMAIN.replace('EXPR', 'adjust(p)'),
    }
    for stage, body in bodies.items():
        p = dict(format=original.FORMAT, root='root.hlsl', entry='main', profile=stage+'_5_0',
                 flags=2048, defines=[dict(name='AMOUNT', value='2')], include_dirs=[],
                 files=[dict(name='root.hlsl', text='#include "common.hlsl"\n'+body),
                        dict(name='common.hlsl', text='float4 adjust(float4 x){return x*AMOUNT;}\n')])
        add('compile', p)
    sample = args.reference/'analysis/capture_samples/shader_project'
    manifest = json.loads((sample/'manifest.json').read_text())
    capture = sample/manifest['file']
    assert hashlib.sha256(capture.read_bytes()).hexdigest() == manifest['sha256']
    from frame import Frame
    with Frame(capture) as frame:
        code = frame.shader(frame.resource(manifest['shader_id'])['data_id'])
    sources = shader_sources.extract(shaders.chunks(code))
    recovered = original.from_sources(sources, 'ps_5_0')
    add('sources', sources, profile='ps_5_0')
    recovered['root'] = manifest['source_project']['root']
    add('compile', recovered)
    add('verify', recovered, bytecode=code.hex())
    wrong = copy.deepcopy(recovered)
    wrong['defines'][0]['value'] = '0.75'
    add('verify', wrong, bytecode=code.hex())
    disk = args.out.resolve() / 'disk.hlsl'
    disk.write_text('#define VALUE 1', encoding='utf-8')
    for include in ['missing.hlsl', str(disk).replace('\\', '/'), 'value.hlsl']:
        p = copy.deepcopy(base)
        if include == 'value.hlsl':
            p['files'][1]['text'] = '#include "value.hlsl"'
        else:
            p['files'][0]['text'] = p['files'][0]['text'].replace('value.hlsl', include)
        add('compile', p)
    input_path, output_path = args.out / 'jobs.json', args.out / 'native.json'
    input_path.write_text(json.dumps(jobs, ensure_ascii=True), encoding='utf-8')
    env = dict(os.environ)
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env.get('PATH', '')
    subprocess.run([str(args.probe.resolve()), '--probe', str(input_path.resolve()), str(output_path.resolve())],
                   env=env, check=True, timeout=180)
    native = json.loads(output_path.read_text(encoding='utf-8'))
    results = []
    for i, (job, actual) in enumerate(zip(jobs, native, strict=True)):
        action, value = job['action'], job['value']
        try:
            if action == 'path': expected = original.path_key(value)
            elif action == 'defines': expected = original.parse_defines(value)
            elif action == 'sources': expected = original.from_sources(value, job['profile'])
            elif action == 'validate':
                p = original.validate(value)
                expected = dict(project=p, sha256=original.digest(p))
            elif action == 'verify':
                expected = original.verify(bytes.fromhex(job['bytecode']), value)
            elif action == 'compile':
                code, report = original.compile_project(value)
                expected = dict(report=report, bytecode=code.hex(), verify=original.verify(code, value))
            reference = dict(ok=True, result=expected)
        except Exception as exc:
            reference = dict(ok=False, error=str(exc))
        comparison = 'exact'
        if action == 'compile' and actual['ok'] and reference['ok']:
            a, b = copy.deepcopy(actual['result']), copy.deepcopy(reference['result'])
            ac, bc = bytes.fromhex(a.pop('bytecode')), bytes.fromhex(b.pop('bytecode'))
            if value.get('flags', 2048) & 1:
                # D3DCompiler embeds a per-compilation PDB GUID/time. Compare all
                # execution/reflection chunks, plus decoded source/environment,
                # and independently verify each compiler result against its project.
                ca, cb = shaders.chunks(ac), shaders.chunks(bc)
                source_equal = shader_sources.extract(ca) == shader_sources.extract(cb)
                for debug in ('SPDB', 'SDBG'):
                    ca.pop(debug, None)
                    cb.pop(debug, None)
                byte_equal = ca == cb and source_equal
                comparison = 'exact non-debug chunks and decoded embedded sources'
            else:
                byte_equal = ac == bc
            passed = byte_equal and a == b
        else:
            passed = actual['ok'] == reference['ok'] and (not reference['ok'] or actual['result'] == reference['result'])
        row = dict(index=i, action=action, passed=passed, rejected=not reference['ok'])
        row['comparison'] = comparison
        if not passed:
            row.update(native=actual, reference=reference)
        results.append(row)
    cli_checks = []
    if args.cli:
        for label, flags in [('captured-debug', 5), ('captured-release', 2048)]:
            p = copy.deepcopy(recovered)
            p['flags'] = flags
            source = args.out/(label+'.json')
            source.write_text(json.dumps(p), encoding='utf-8')
            native_dir, reference_dir = args.out/(label+'-native'), args.out/(label+'-reference')
            subprocess.run([str(args.cli.resolve()), 'compile-project', str(capture.resolve()),
                            '--id', str(manifest['shader_id']), '--source', str(source.resolve()),
                            '--out', str(native_dir.resolve())], env=env, check=True, capture_output=True, timeout=60)
            subprocess.run([sys.executable, '-I', str((args.reference/'standalone/analyze.py').resolve()),
                            str(capture.resolve()), 'compile-project', '--id', str(manifest['shader_id']),
                            '--source', str(source.resolve()), '--out', str(reference_dir.resolve())],
                           check=True, capture_output=True, timeout=60)
            a = json.loads((native_dir/'report.json').read_text('utf-8'))
            b = json.loads((reference_dir/'result.json').read_text('utf-8'))
            keys = ['compilation', 'resource_id', 'profile', 'stage', 'pipeline_stage',
                    'bytecode_stage', 'passthrough', 'class_linkage_id', 'interface_slots']
            checks = dict(metadata=all(a[k] == b[k] for k in keys),
                          project=json.loads((native_dir/'shader_project.json').read_text()) ==
                                  json.loads((reference_dir/'shader_project.json').read_text()),
                          assembly=(native_dir/'replacement.asm').read_bytes() ==
                                   (reference_dir/'replacement.asm').read_bytes())
            ac, bc = (native_dir/'replacement.dxbc').read_bytes(), (reference_dir/'replacement.dxbc').read_bytes()
            if flags & 1:
                ca, cb = shaders.chunks(ac), shaders.chunks(bc)
                checks['embedded_sources'] = shader_sources.extract(ca) == shader_sources.extract(cb)
                for debug in ('SPDB', 'SDBG'):
                    ca.pop(debug, None)
                    cb.pop(debug, None)
                checks['bytecode_non_debug'] = ca == cb
            else:
                checks['bytecode_exact'] = ac == bc
            cli_checks.append(dict(case=label, passed=all(checks.values()), checks=checks))
    summary = dict(passed=all(r['passed'] for r in results+cli_checks), cases=len(results),
                   rejected=sum(r['rejected'] for r in results),
                   reference_sha256=hashlib.sha256((args.reference/'standalone/shader_project.py').read_bytes()).hexdigest(),
                   results=results, cli_checks=cli_checks)
    (args.out/'validation.json').write_text(json.dumps(summary, indent=2, ensure_ascii=True), encoding='utf-8')
    print(json.dumps({k: v for k, v in summary.items() if k != 'results'}))
    for r in results:
        if not r['passed']:
            print('FAIL', r['index'], r['action'], str(r)[:1200])
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
