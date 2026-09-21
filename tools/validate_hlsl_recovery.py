"""Development-only, exact native DXBC lowering comparison with the Python oracle."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys
from unittest.mock import patch


def metadata(stage):
    return dict(stage=stage, profile=stage+'_5_0', bindings=[], constant_buffers=[], signatures={})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--qt-bin', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cli', type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.reference/'standalone'))
    import hlsl_recover as original
    import shaders
    from frame import Frame
    jobs = []

    def add(action, **kw):
        jobs.append(dict(action=action, **kw))

    hint = '// FloraGPA compiler optimization: preserve'
    for source in ['', hint, hint+'\rfloat4 main():SV_Target{return 1;}',
                   '\n'*4+hint, '\n'*5+hint, ' '+hint, hint+' ', hint+'\r\nbody',
                   '\v'+hint, '\x85'+hint]:
        for optimization in ['auto', 'preserve', 'optimize', 'invalid']:
            add('options', source=source, optimization=optimization)
    for entry in ['main', 'edited']:
        source = 'float4 '+entry+'():SV_Target{return float4(1,0,0,1);}'
        code = shaders.compile_hlsl(source, 'ps_5_0', entry)[0]
        for text in [source, source.replace('(1,0,0,1)', '(0,1,0,1)'), 'invalid source']:
            add('reconstruct', bytecode=code.hex(), saved=dict(source_language='hlsl', source_text=text, source_entry=entry))

    for text in ['', 'r0.xyzw, r1.x', 'r0.x, cb0[r1.x + 1].xyzw, l(0x00000000)',
                 'a,(b,c),[d,e],f', 'a,,', 'x\u2003,\t y']:
        add('split', text=text)
    for text in ['ret', 'mov r0.xyzw, r1.xyzw', 'mad [precise] r0.x, r1.x, r2.x, r3.x',
                 'sample_indexable [precise(xy)](texture2d)(float,float,float,float) r0.xyzw, r1.xyzw, t0.xyzw, s0',
                 'sincos [precise] r0.xy, r1.xy, r2.xyzw']:
        add('parse', text=text)
    operands = ['r0', 'r0.x', '-r0.x', '|r1.wzyx|', '-|r2.x|', 'r1.xy', 'r1.zzzz',
                'cb3[r0.x + 1].xyzw', 'x0[r0.y * 2 - 1].xxxx', 'vicp[2][1].xyzw',
                'v[2][0].xyzw', 'vThreadID.x', 'vGSInstanceID.x', 'vPrim', 'vDomain.xyzx',
                'l(0x80000000)', 'l(0x00000000, 0x3f800000, 0x7fc01234, 0xffffffff)',
                'l(1.0)', 'l(0x0)', 'l(0x00000000,0x00000001)', 'cb0[abc].xyzw', 'r0.q', '']
    for operand in operands:
        for kind in ['bits', 'f', 'i', 'u']:
            add('raw', operand=operand, kind=kind)
    for stage in ['vs', 'ps', 'gs', 'hs', 'ds', 'cs']:
        for kind in ['bits', 'f', 'i', 'u']:
            for dest in ['null', 'r0.xy', 'o0.xyzw', 'oDepth', 'x0[r1.x + 1].zw', 'v0.x']:
                for sat in [False, True]:
                    add('write', stage=stage, dest=dest, expression='mad(asfloat(r0),asfloat(vDomain),asfloat(vicp[0][0]))', kind=kind, sat=sat)
    declarations = ['dcl_globalFlags refactoringAllowed', 'dcl_globalFlags refactoringAllowed | skipOptimization',
                    'dcl_constantbuffer CB2[13], dynamicIndexed', 'dcl_temps 7',
                    'dcl_indexableTemp x0[3], 2', 'dcl_indexableTemp x0[0], 4', 'dcl_indexableTemp x0[4097], 4',
                    'dcl_thread_group 8, 4, 1', 'dcl_input vThreadID.xyz',
                    'dcl_immediateConstantBuffer { { 0x00000000, 0x3f800000, 0x00000000, 0xffffffff } }',
                    'dcl_immediateConstantBuffer { 1.0 }', 'dcl_resource_raw t0', 'dcl_uav_raw u0',
                    'dcl_resource_structured t0, 12', 'dcl_uav_structured u0, 16',
                    'dcl_tgsm_structured g0, 12, 64', 'dcl_tgsm_raw g0, 256',
                    'dcl_resource_structured t0, 3', 'dcl_sampler s0, mode_default',
                    'dcl_sampler s1, mode_comparison', 'dcl_inputprimitive triangle',
                    'dcl_maxout 3', 'dcl_maxout 1025', 'dcl_gsinstances 0', 'dcl_gsinstances 32',
                    'dcl_stream m3', 'dcl_outputtopology pointlist', 'dcl_input v[3][0].xyzw',
                    'dcl_input vPrim', 'dcl_input vGSInstanceID', 'dcl_input_ps linear centroid v0.xy',
                    'dcl_output oDepthGE', 'dcl_tessellator_domain domain_tri',
                    'dcl_hs_max_tessfactor l(0x42800000)', 'dcl_output_control_point_count 3', 'dcl_unknown 1']
    for stage in ['vs', 'ps', 'gs', 'hs', 'ds', 'cs']:
        for text in declarations:
            add('declaration', stage=stage, text=text)
    unary = 'f16tof32 f32tof16 deriv_rtx deriv_rty deriv_rtx_coarse deriv_rty_coarse deriv_rtx_fine deriv_rty_fine sqrt rsq rcp frc exp log round_ni round_pi round_z round_ne mov not ineg utof itof ftou ftoi'.split()
    binary = 'add mul div min max dp2 dp3 dp4 eq ne lt ge ieq ine ilt ige ult uge iadd and or xor ishl ishr ushr imin imax umin umax'.split()
    ternary = 'mad movc imad umad ubfe ibfe'.split()
    for stage in ['vs', 'ps', 'ds', 'cs']:
        for precise in ['', ' [precise(xy)]']:
            for op, count in [(x, 1) for x in unary] + [(x, 2) for x in binary] + [(x, 3) for x in ternary] + [('bfi', 4)]:
                for suffix in ['', '_sat']:
                    add('instruction', stage=stage, text=op+suffix+precise+' r0.xy, '+', '.join(['r1.xyzw', '-r2.wzyx', 'r3.x', 'l(0xffffffff)'][:count]))
            for op in ['imul', 'umul', 'udiv', 'sincos']:
                for dest in ['r0.xy', 'null']:
                    add('instruction', stage=stage, text=op+precise+' '+dest+', r1.xy, r0.xyzw'+('' if op=='sincos' else ', r1.wzyx'))
        for op in ['if_nz', 'if_z', 'breakc_nz', 'breakc_z', 'retc_nz', 'retc_z', 'discard_z', 'discard_nz']:
            add('instruction', stage=stage, text=op+' r0.x')
        for op in ['else', 'endif', 'loop', 'endloop', 'break', 'ret', 'unsupported', 'emit', 'cut']:
            add('instruction', stage=stage, text=op)
    for dimension in ['texture2d', 'texture2darray', 'texture3d', 'texturecube', 'texturecubearray', 'buffer']:
        for kind in ['float', 'uint', 'sint']:
            for access, register in [('resource', 't0'), ('uav_typed', 'u0')]:
                decl = 'dcl_'+access+'_'+dimension+' ('+','.join([kind]*4)+') '+register
                add('declaration', text=decl)
                samplers = ['dcl_sampler s0, mode_default', 'dcl_sampler s1, mode_comparison']
                decls = [decl]+samplers
                for opcode, tail in [('ld_indexable', ''), ('ld_uav_typed_indexable', ''),
                                     ('sample_indexable', ', s0'), ('sample_d_indexable', ', s0, r2.xyzw, r3.xyzw'),
                                     ('sample_l_indexable', ', s0, r2.x'), ('sample_c_lz_indexable', ', s1, r2.x'),
                                     ('gather4_indexable', ', s0.y'), ('gather4_c_indexable', ', s1.x, r2.x')]:
                    add('instruction', stage='ps', declarations=decls,
                        text=opcode+'('+dimension+')('+','.join([kind]*4)+') r0.xyzw, r1.xyzw, '+register+('.xxxx' if opcode=='sample_c_lz_indexable' else '.xyzw')+tail)
                add('instruction', declarations=decls, text='store_uav_typed '+register+'.xyzw, r0.xyzw, r1.xyzw')
        for offset in ['(-8,7,0)', '(1,2,3)', '(8,0,0)', '(0,-9,0)', '(0,0,0)']:
            add('offset', text='sample_l_aoffimmi_indexable'+offset+'('+dimension+')', dimension=dimension)
    for decl in ['dcl_resource_raw t0', 'dcl_uav_raw u0', 'dcl_resource_structured t0, 12',
                 'dcl_uav_structured u0, 16', 'dcl_tgsm_raw g0, 256', 'dcl_tgsm_structured g0, 16, 64']:
        resource = decl.split()[1].rstrip(',')
        structured = 'structured' in decl
        decls = [decl]
        for mask in ['xyzw', 'x', 'yw']:
            add('instruction', declarations=decls, text=('ld_structured r0.'+mask+', r1.x, r2.x, ' if structured else 'ld_raw r0.'+mask+', r1.x, ')+resource+'.wzyx')
            add('instruction', declarations=decls, text=('store_structured ' if structured else 'store_raw ')+resource+'.'+mask+', r1.x, '+('r2.x, ' if structured else '')+'r3.xyzw')
        for op in ['atomic_iadd', 'imm_atomic_iadd']:
            add('instruction', declarations=decls, text=op+' '+('r0.x, ' if op.startswith('imm') else '')+resource+', r1.xyzw, r2.x')
    for op in ['sync_g', 'sync_g_t', 'sync_ugroup', 'sync_uglobal', 'sync_uglobal_g_t', 'sync_t', 'sync_unknown']:
        add('instruction', text=op)
    for binding_type in [9, 10, 11]:
        info = metadata('cs')
        info['bindings'] = [dict(slot=0, type=binding_type)]
        for stride in [4, 8, 12, 16, 20]:
            decl = ['dcl_uav_structured u0, '+str(stride)]
            add('declaration', info=info, text=decl[0])
            for op in ['imm_atomic_alloc', 'imm_atomic_consume']:
                add('instruction', info=info, declarations=decl, text=op+' r0.x, u0')
            body = ['imm_atomic_alloc r0.x, u0', 'store_structured u0.'+'xyzw'[:stride//4]+', r0.x, l(0x00000000), r1.xyzw']
            add('prepare', info=info, declarations=decl, body=body)
            add('prepare', info=info, declarations=decl, body=body+['mov r2.x, r0.x'])
    # Deterministic operand and precise-write combinations catch evaluation-order drift.
    rng = random.Random(43917)
    for _ in range(250):
        add('write', stage=rng.choice(['ps', 'vs', 'cs', 'ds']), dest=rng.choice(['r0.x', 'r4.zw', 'null']),
            expression='expression', kind=rng.choice(['bits', 'f', 'i']), serial=rng.randrange(100),
            precise=rng.choice([True, False]), ordered_sample_mad=rng.choice([True, False]),
            current_opcode=rng.choice(['mad', 'mad_sat', 'mov']))

    corpus = {}
    for capture in sorted(args.reference.glob('*.gpa_frame')):
        with Frame(capture) as frame:
            for entry in frame.entries.values():
                if entry.category == 9 and entry.type == 0x81:
                    code = frame.shader(entry.id)
                    key = hashlib.sha256(code).hexdigest()
                    corpus.setdefault(key, dict(code=code, origins=[]))['origins'].append(capture.name+':'+str(entry.id))
    # All shader stages from captured fixtures, including GS and system signatures.
    for capture in sorted((args.reference/'analysis/capture_samples').rglob('*.gpa_frame')):
        with Frame(capture) as frame:
            for entry in frame.entries.values():
                if entry.category == 9 and entry.type == 0x81:
                    code = frame.shader(entry.id)
                    key = hashlib.sha256(code).hexdigest()
                    corpus.setdefault(key, dict(code=code, origins=[]))['origins'].append(str(capture.relative_to(args.reference))+':'+str(entry.id))
    for key, row in corpus.items():
        add('recover', bytecode=row['code'].hex(), corpus_key=key)
        add('reconstruct', bytecode=row['code'].hex(), corpus_key=key)
    env = dict(os.environ)
    env['PATH'] = str(args.qt_bin.resolve())+os.pathsep+env.get('PATH', '')

    def reference(job):
        action = job['action']
        if action == 'split': return original.split_args(job['text'])
        if action == 'parse': return list(original.parse_instruction(job['text']))
        if action == 'recover': return original.recover(bytes.fromhex(job['bytecode']))
        if action == 'options': return shaders.compilation_options(job['source'], job.get('optimization', 'auto'))
        if action == 'reconstruct':
            code = bytes.fromhex(job['bytecode'])
            profile = shaders.inspect(code)['profile']
            saved, entry = False, 'main'
            if job.get('saved', {}).get('source_language') == 'hlsl':
                draft = job['saved']
                try:
                    entry = draft.get('source_entry', 'main')
                    source = draft['source_text']
                    rebuilt, diagnostics = shaders.compile_hlsl(source, profile, entry)
                    saved = rebuilt == code
                except ValueError:
                    pass
            if not saved:
                source = original.recover(code)
                rebuilt, diagnostics = shaders.compile_hlsl(source, profile)
            report = dict(source_kind='saved_applied_hlsl' if saved else 'reconstructed_hlsl_not_original',
                          tool='FloraGPA experiment source' if saved else 'FloraGPA native DXBC lowering',
                          native=True, recompiles=True, diagnostics=diagnostics,
                          compilation=shaders.compilation_options(source),
                          semantic_equivalence='bytecode_identical' if saved else 'not_verified',
                          source_sha256=hashlib.sha256(source.encode('utf-8')).hexdigest())
            if saved: report['entry'] = entry
            return dict(source=source, bytecode=rebuilt.hex(), report=report)
        with patch.object(original, 'inspect', return_value=copy.deepcopy(job.get('info', metadata(job.get('stage', 'cs'))))), \
             patch.object(original, 'disassemble', return_value=job.get('assembly', '')):
            lower = original.Lowerer(b'')
        lower.return_code = job.get('return_code', 'return;')
        for line in job.get('declarations', []):
            if not lower.declaration(line): raise ValueError('Unsupported declaration')
        lower.serial = job.get('serial', 0)
        lower.current_opcode = job.get('current_opcode', '')
        lower.ordered_sample_mad = job.get('ordered_sample_mad', False)
        lower.instruction_precise = job.get('precise', False)
        if action == 'raw': return lower.raw(job['operand'], job.get('kind', 'bits'))
        if action == 'write': result = lower.write(job['dest'], job['expression'], job.get('kind', 'bits'), job.get('sat', False))
        elif action == 'declaration': result = lower.declaration(job['text'])
        elif action == 'instruction': result = lower.instruction(job['text'])
        elif action == 'prepare': return lower.memory.prepare(job['body'])
        elif action == 'graphics': result = list(lower.graphics.setup())
        elif action == 'geometry': result = list(lower.geometry.setup())
        elif action == 'translate': return lower.translate()
        elif action == 'offset': return lower.sample_offset(job['text'], job['dimension'])
        return dict(result=result, globals=lower.globals, serial=lower.serial, return_code=lower.return_code)

    results = []
    source_dir = args.out/'sources'
    source_dir.mkdir()
    for start in range(0, len(jobs), 100):
        batch = jobs[start:start+100]
        input_path, output_path = args.out/f'jobs-{start}.json', args.out/f'native-{start}.json'
        input_path.write_text(json.dumps(batch, ensure_ascii=True), encoding='utf-8')
        subprocess.run([str(args.probe.resolve()), '--probe', str(input_path.resolve()), str(output_path.resolve())], env=env, check=True, timeout=180)
        native = json.loads(output_path.read_text('utf-8'))
        for i, (job, actual) in enumerate(zip(batch, native, strict=True), start):
            try: expected = dict(ok=True, result=reference(job))
            except Exception as exc: expected = dict(ok=False, error=str(exc))
            passed = actual['ok']==expected['ok'] and (not expected['ok'] or actual['result']==expected['result'])
            row = dict(index=i, action=job['action'], passed=passed, rejected=not expected['ok'])
            if not passed: row.update(native=actual, reference=expected)
            if job['action']=='recover':
                key = job['corpus_key']
                row.update(sha256=key, origins=corpus[key]['origins'])
                if actual['ok']:
                    (source_dir/(key+'.hlsl')).write_bytes(actual['result'].encode('utf-8'))
                    try:
                        profile = shaders.inspect(corpus[key]['code'])['profile']
                        code, diagnostics = shaders.compile_hlsl(actual['result'], profile)
                        (source_dir/(key+'.dxbc')).write_bytes(code)
                        row['recompiles'] = True
                    except Exception as exc:
                        row.update(recompiles=False, compilation_error=str(exc))
                else: row['native_error'] = actual['error']
            results.append(row)
        print(f'{min(start+100, len(jobs))}/{len(jobs)} cases; {sum(not r["passed"] for r in results)} differences', flush=True)
    cli_checks = []
    if args.cli:
        supported = {r['sha256'] for r in results if r['action']=='recover' and not r['rejected']}
        selected = set()
        captures = sorted(args.reference.glob('*.gpa_frame')) + sorted((args.reference/'analysis/capture_samples').rglob('*.gpa_frame'))
        for capture in captures:
            with Frame(capture) as frame:
                for e in frame.entries.values():
                    if e.category != 5 or not 0x90 <= e.type <= 0x95:
                        continue
                    resource = frame.resource(e.id)
                    if not resource['data_id']:
                        continue
                    raw = frame.shader(resource['data_id'])
                    if hashlib.sha256(raw).hexdigest() not in supported:
                        continue
                    stage = shaders.inspect(raw)['stage']
                    if stage in selected:
                        continue
                    selected.add(stage)
                    native_dir, reference_dir = args.out/('cli-'+stage), args.out/('reference-'+stage)
                    subprocess.run([str(args.cli.resolve()), 'shader', str(capture.resolve()), '--id', str(e.id), '--recover', '--out', str(native_dir.resolve())], env=env, check=True, capture_output=True, timeout=90)
                    subprocess.run([sys.executable, '-I', str((args.reference/'standalone/analyze.py').resolve()), str(capture.resolve()), 'shader', '--id', str(e.id), '--recover', '--out', str(reference_dir.resolve())], check=True, capture_output=True, timeout=90)
                    actual = json.loads((native_dir/'shader.json').read_text('utf-8'))['decompilation']
                    expected = json.loads((reference_dir/'result.json').read_text('utf-8'))['decompilation']
                    checks = dict(report=actual==expected)
                    for name in ['reconstructed.dxbc', 'reconstructed.hlsl', 'reconstructed.recompiled.dxbc']:
                        checks[name] = (native_dir/name).read_bytes() == (reference_dir/name).read_bytes()
                    cli_checks.append(dict(stage=stage, capture=str(capture), resource=e.id, checks=checks, passed=all(checks.values())))
            if len(selected)==6:
                break
        if selected != {'vs', 'ps', 'gs', 'hs', 'ds', 'cs'}:
            raise RuntimeError('Missing stage in CLI coverage: '+str(selected))
    summary = dict(passed=all(r['passed'] for r in results+cli_checks), cases=len(results), rejected=sum(r['rejected'] for r in results),
                   corpus=len(corpus), recovered=sum(r['action']=='recover' and not r['rejected'] for r in results),
                   recompiles=sum(r.get('recompiles', False) for r in results),
                   reference_sha256={name: hashlib.sha256((args.reference/'standalone'/name).read_bytes()).hexdigest()
                                     for name in ['hlsl_recover.py', 'hlsl_memory.py', 'hlsl_graphics.py', 'hlsl_geometry.py']}, results=results, cli_checks=cli_checks)
    (args.out/'validation.json').write_text(json.dumps(summary, indent=2, ensure_ascii=True), encoding='utf-8')
    print(json.dumps({k:v for k,v in summary.items() if k!='results'}), flush=True)
    for row in results:
        if not row['passed']: print('FAIL', row['index'], row['action'], str(row)[:700])
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
