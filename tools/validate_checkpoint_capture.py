"""Development-only comparison of native checkpoint capture and complete exports.

SPDB/SDBG symbols, original source lines, source stacks and HS phase ownership are
compared. Qt interactions and configuration have separate validators. Compare other report fields,
original bytecode/disassembly, and complete per-invocation histories (including
raw register bits and CSVs). Atomic invocation/record allocation order may vary;
never sort individual snapshots across invocations or discard their hit order.
"""
import argparse
import collections
import copy
import csv
import hashlib
import json
import math
import os
import struct
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
for name in ('reference', 'exe', 'qt-bin', 'out'):
    p.add_argument('--'+name, type=Path, required=True)
p.add_argument('--smoke', action='store_true')
p.add_argument('--source-only', action='store_true', help='SPDB and SDBG source-mapped production captures')
p.add_argument('--integration-only', action='store_true', help='Indirect draws, scoped buffer edits and disabled captures')
a = p.parse_args()
a.out = a.out.resolve()
a.out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.reference/'standalone'), str(a.reference/'tools')]
from frame import Frame
from engine import Engine
from dx11 import Device
from gs_checkpoint import export, catalog, headers, register_values
from native_invocation_selector import from_snapshot, keys
from dxbc_hull_phases import layout
from dxbc_gs_checkpoint import program
from validate_gs_calls import make_fixture as calls_fixture, with_so
from validate_gs_indexable import make_fixture as array_fixture
from validate_ds_checkpoints import fixture as ds_fixture
from validate_hs_checkpoints import fixture as hs_fixture, join_fixture
from validate_geometry_instances import fixture as instances_fixture
from validate_buffer_edits import experiment, operation
from experiments import Experiment

checks = []
env = {k: v for k, v in os.environ.items() if k.upper() in
       ('SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'APPDATA', 'LOCALAPPDATA')}
env['PATH'] = str(a.qt_bin.resolve()) + os.pathsep + os.environ['WINDIR'] + '/System32;' + os.environ['WINDIR']

def save(completed=False):
    sources = ('gs_checkpoint.py', 'vertex_writes.py', 'native_invocation_selector.py', 'dxbc_gs_checkpoint.py')
    (a.out/'validation.json').write_text(json.dumps(dict(
        completed=completed, passed=all(c['passed'] for c in checks), checks=checks,
        outside_scope=['Debugger configuration', 'Qt checkpoint integration'],
        executable_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),
        reference_sources={s: hashlib.sha256((a.reference/'standalone'/s).read_bytes()).hexdigest() for s in sources}
    ), indent=2), encoding='utf-8')

def table(path):
    with path.open(encoding='utf-8-sig', newline='') as file:
        return list(csv.DictReader(file))

def numeric(value):
    try:
        n = float(value)
        return value.lower() if math.isnan(n) else n
    except ValueError:
        return value

def canonical(folder, report):
    meta = report['register_capture']
    data = (folder/'snapshots.bin').read_bytes()
    assert len(data) == meta['records'] * meta['record_stride']
    assert hashlib.sha256(data).hexdigest() == meta['sha256']
    rows = headers(data, meta)
    instructions = {e['token']: e['instruction'] for e in report['catalog']}
    for row in rows:
        if meta.get('trace'):
            row['instruction'] = instructions[row['token']]
            for frame in row.get('call_stack', []):
                frame['call_instruction'] = instructions[frame['call_token']]
    assert report['hits_preview'] == rows[:10000]
    assert report['preview_truncated'] == (len(rows) > 10000)
    assert report['record_count'] == len(rows)
    hit_rows = table(folder/'hits.csv')
    assert [int(r['record']) for r in hit_rows] == [r['record'] for r in rows]
    reg_rows = collections.defaultdict(list)
    for row in table(folder/'registers.csv'):
        record = int(row.pop('record'))
        reg_rows[record].append({k: numeric(v) if k == 'float' else v for k, v in row.items()})
    assert len(reg_rows) == (len(rows) if meta['registers'] else 0)
    groups = collections.defaultdict(list)
    for row, csv_hit in zip(rows, hit_rows):
        record = row['record']
        regs = register_values(data, meta, record)
        assert len(reg_rows[record]) == len(regs)*4
        # Unwritten storage is retained too: instrumented buffers are zero-initialized.
        groups[row['invocation']].append(dict(
            header={k: v for k, v in row.items() if k not in ('record', 'invocation')},
            registers=regs,
            hits_csv={k: numeric(v) for k, v in csv_hit.items() if k not in ('record', 'invocation')},
            registers_csv=reg_rows[record]))
    return sorted((json.dumps(group, sort_keys=True) for group in groups.values()))

def core(report):
    result = copy.deepcopy(report)
    for key in ('source_debug_status', 'hits_preview'):
        result.pop(key, None)
    if 'register_capture' in result:
        meta = result['register_capture']
        for key in ('sha256', 'full_capture_sha256'):
            meta.pop(key)
    return result

def compare(name, path, stage, driver, instruction=None, trace=False, phase=None, selector=None, reject=False,
            project=None, suppress=False):
    expected, actual = a.out/(name+'-python'), a.out/(name+'-native')
    error = None
    with Frame(path) as frame:
        device = Device(driver)
        try:
            try:
                want = export(Engine(frame, device, suppress_draws=suppress,
                                     experiment=Experiment(frame, project) if project else None), 100, expected, checkpoint=instruction,
                              trace=trace, shader_stage=stage, input_selector=selector, hs_phase=phase)
            except (ValueError, RuntimeError, KeyError) as exc:
                error = str(exc)
        finally:
            device.close()
    args = [str(a.exe.resolve()), 'shader-checkpoint', str(path.resolve()), '--event', '100',
            '--shader-stage', stage, '--out', str(actual)]
    if driver == 'warp': args += ['--warp']
    if project: args += ['--experiment', str(project)]
    if suppress: args += ['--suppress-draws']
    if trace: args += ['--trace']
    if instruction is not None: args += ['--instruction', str(instruction)]
    if phase is not None: args += ['--hs-phase', str(phase)]
    if selector is not None:
        filename = a.out/(name+'-selector.json')
        filename.write_text(json.dumps(selector), encoding='utf-8')
        args += ['--input-selector', str(filename)]
    run = subprocess.run(args, env=env, capture_output=True, timeout=180)
    (a.out/(name+'.log')).write_bytes(run.stdout+run.stderr)
    differences = []
    raw_equal = None
    if bool(error) != reject:
        differences.append(dict(reference_error=error, expected_rejection=reject))
    if error:
        if run.returncode != 1: differences.append(dict(rejection=run.returncode))
    elif run.returncode:
        differences.append(dict(native_error=run.stderr.decode(errors='replace')))
    else:
        got = json.loads((actual/'checkpoint.json').read_text())
        want = json.loads(json.dumps(want))
        left, right = core(want), core(got)
        for field in sorted(set(left)|set(right)):
            if left.get(field) != right.get(field):
                differences.append(dict(field=field, expected=left.get(field), actual=right.get(field)))
        for file in ('shader.dxbc', 'shader.asm'):
            # Python text writes translate LF to Windows CRLF; compare disassembly text.
            x, y = ((expected/file).read_text(), (actual/file).read_text()) if file.endswith('.asm') else ((expected/file).read_bytes(), (actual/file).read_bytes())
            if x != y: differences.append(dict(file=file))
        if 'register_capture' in want:
            raw_equal = (expected/'snapshots.bin').read_bytes() == (actual/'snapshots.bin').read_bytes()
            try:
                if canonical(expected, want) != canonical(actual, got):
                    differences.append(dict(records='Complete invocation histories or CSV exports differ'))
            except (AssertionError, ValueError, KeyError) as exc:
                differences.append(dict(validation=repr(exc)))
        modules = [Path(m).name.lower() for m in json.loads((actual/'report.json').read_text())['loaded_modules']]
        if any(x.startswith(('python', 'tk8', 'tcl8', 'gpa_', 'gpa-')) or x in
               ('renderdoc.dll', 'dx11_player.dll', 'dx11_playback.dll', 'shimd3d64.dll', 'gpa.dll') for x in modules):
            differences.append(dict(runtime=modules))
    checks.append(dict(name=name, passed=not differences, expected_rejection=reject,
                       reference_error=error, raw_allocation_equal=raw_equal, differences=differences))
    save()
    print(name, 'PASS' if not differences else 'FAIL', flush=True)
    return (want, expected) if not error else (None, expected)

cases = []
if a.integration_only:
    for indexed in (False, True):
        for stage in ('gs', 'hs', 'ds'):
            label = stage+'-indirect-'+str(indexed)
            path = instances_fixture(a.out/(label+'.gpa_frame'), kind='gs' if stage == 'gs' else 'tess',
                                     indexed=indexed, indirect=True, instances=3)
            with Frame(path) as frame:
                raw = frame.shader({'gs': 13, 'hs': 61, 'ds': 63}[stage])
                phase = layout(program(raw, stage)[4])[0]['id'] if stage == 'hs' else None
                edited = a.out/(label+'-input.json')
                experiment(frame, edited, [operation(24, 0, struct.pack('<32I', *range(500, 532)))])
                args_edited = a.out/(label+'-args.json')
                experiment(frame, args_edited, [operation(30, 8, struct.pack('<I', 2))])
                disabled = a.out/(label+'-disabled.json')
                experiment(frame, disabled, [dict(kind='enabled', event=100, value=False)])
            for driver in ('hardware', 'warp'):
                for suffix, project, suppress in [('original', None, False), ('input', edited, False),
                                                  ('args', args_edited, False), ('disabled', disabled, False),
                                                  ('suppress', None, True)]:
                    compare(label+'-'+driver+'-'+suffix, path, stage, driver, trace=True, phase=phase,
                            project=project, suppress=suppress)
    save(True)
    raise SystemExit(0 if all(c['passed'] for c in checks) else 1)
if a.source_only:
    from validate_gs_source_lines import PROJECT
    from sdbg_tess_fixture import fixture as legacy_tess, compile_legacy
    from validate_geometry_emissions import fixture as geometry_fixture
    from validate_geometry_emissions_edges import replace
    from shader_project import compile_project
    from probe_hs_source_scopes import project as hull_project
    from validate_native_source_variables import TYPED
    from validate_gs_source_stack import nested
    a.smoke = True
    for legacy in (False, True):
        tag = 'sdbg' if legacy else 'spdb'
        path = a.out/(tag+'-gs.gpa_frame')
        geometry_fixture(path, topology='point')
        raw = compile_legacy(PROJECT) if legacy else compile_project(PROJECT)[0]
        replace(path, path, raw, data_id=81)
        cases.append((tag+'-gs', path, 'gs', 81))
        if legacy:
            path = a.out/'sdbg-tess.gpa_frame'; legacy_tess(path)
            cases += [('sdbg-hs', path, 'hs', 61), ('sdbg-ds', path, 'ds', 63)]
        else:
            path = a.out/'spdb-hs.gpa_frame'; hs_fixture(path)
            replace(path, path, compile_project(hull_project(5))[0], data_id=61)
            cases.append(('spdb-hs', path, 'hs', 61))
            path, _, _ = ds_fixture(a.out/'spdb-ds.gpa_frame')
            cases.append(('spdb-ds', path, 'ds', 83))
    for tag, text in [('typed', TYPED), ('unused', TYPED.replace(
            'a[(prim+gi)%3].x+a[(prim+gi)%3].y+a[(prim+gi)%3].z', 'a[(prim+gi)%3].x'))]:
        path = a.out/('spdb-'+tag+'.gpa_frame')
        geometry_fixture(path, topology='point')
        project = dict(format='FloraGPA shader project 1', files=[dict(name='types.hlsl', text=text)],
                       root='types.hlsl', entry='main', profile='gs_5_0', flags=5)
        replace(path, path, compile_project(project)[0], data_id=81)
        cases.append(('spdb-'+tag, path, 'gs', 81))
    for flags in (1, 5):
        path = a.out/('spdb-nested-'+str(flags)+'.gpa_frame')
        geometry_fixture(path, topology='point')
        replace(path, path, compile_project(nested(flags))[0], data_id=81)
        cases.append(('spdb-nested-'+str(flags), path, 'gs', 81))
    from sdbg_variable_fixture import SOURCE as LEGACY_SOURCE, TYPED as LEGACY_TYPED
    from sdbg_array_fixture import SOURCE as LEGACY_ARRAY
    from probe_sdbg_wide import PREFIX, CASES
    legacy_sources = [('basic', LEGACY_SOURCE), ('typed', LEGACY_TYPED), ('array', LEGACY_ARRAY)]
    for tag, (body, expression) in CASES.items():
        legacy_sources.append(('wide-'+tag, PREFIX+body+'\nV o;o.p=input[0];o.ids=uint4('+expression+',prim,0,1);dst.Append(o);\n}'))
    for tag, text in legacy_sources:
        path = a.out/('sdbg-'+tag+'.gpa_frame')
        geometry_fixture(path, topology='point')
        project = dict(format='FloraGPA shader project 1', files=[dict(name='legacy.hlsl', text=text)],
                       root='legacy.hlsl', entry='main', profile='gs_5_0', flags=5)
        replace(path, path, compile_legacy(project), data_id=81)
        cases.append(('sdbg-'+tag, path, 'gs', 81))
else:
    path = calls_fixture(a.out/'calls')
    cases.append(('calls', path, 'gs', 81))
    if not a.smoke:
        path = with_so(path, a.out/'calls-so.gpa_frame')
        cases.append(('calls-so', path, 'gs', 81))
        for profile in ('gs_4_0', 'gs_4_1', 'gs_5_0'):
            path, _ = array_fixture(a.out/profile, profile)
            cases.append((profile, path, 'gs', 81))
    for domain in (('tri',) if a.smoke else ('tri', 'quad', 'isoline')):
        path, _, _ = ds_fixture(a.out/('ds-'+domain+'.gpa_frame'), domain=domain, instances=2, patches=2)
        cases.append(('ds-'+domain, path, 'ds', 83))
        path = a.out/('hs-'+domain+'.gpa_frame')
        hs_fixture(path, domain=domain)
        cases.append(('hs-'+domain, path, 'hs', 61))
    if not a.smoke:
        path = a.out/'hs-join.gpa_frame'
        join_fixture(path)
        cases.append(('hs-join', path, 'hs', 61))
        path, _, _ = ds_fixture(a.out/'ds-culled.gpa_frame', factor=0)
        cases.append(('ds-culled', path, 'ds', 83))
for label, path, stage, shader in cases:
    with Frame(path) as f: raw = f.shader(shader)
    entries, _ = catalog(raw, stage)
    phases = layout(program(raw, stage)[4]) if stage == 'hs' else [None]
    for driver in ('hardware', 'warp'):
        base = label+'-'+driver
        compare(base+'-catalog', path, stage, driver)
        for phase in phases:
            pid = phase['id'] if phase else None
            prefix = base + (('-phase'+str(pid)) if phase else '')
            want, folder = compare(prefix+'-trace', path, stage, driver, trace=True, phase=pid)
            allowed = [e for e in entries if e['checkpoint_allowed'] and (phase is None or e['hs_phase'] == pid)]
            for entry in (allowed[:1] if a.smoke else allowed[:1]+allowed[-1:]):
                compare(prefix+'-point'+str(entry['instruction']), path, stage, driver, instruction=entry['instruction'], phase=pid)
            if want and want['record_count'] and keys(want['register_capture']['register_slots'], want['register_capture']['known_inputs']):
                data = (folder/'snapshots.bin').read_bytes()
                row = headers(data, want['register_capture'])[0]
                selector = from_snapshot(want, register_values(data, want['register_capture'], row['record']), row, 'all')
                selected, _ = compare(prefix+'-all', path, stage, driver, trace=True, phase=pid, selector=selector)
                if not a.smoke:
                    unique = copy.deepcopy(selector); unique['match_policy'] = 'unique'
                    compare(prefix+'-unique', path, stage, driver, trace=True, phase=pid, selector=unique,
                            reject=selected['register_capture']['matched_invocations'] != 1)
                    absent = copy.deepcopy(selector); absent['inputs'][0]['bits'] ^= 0xffffffff
                    compare(prefix+'-absent', path, stage, driver, trace=True, phase=pid, selector=absent, reject=True)
                    compare(prefix+'-selector-without-trace', path, stage, driver, phase=pid, selector=selector, reject=True)
        if not a.smoke:
            compare(base+'-invalid-instruction', path, stage, driver, instruction=0xffffff, reject=True)
            if stage == 'hs':
                compare(base+'-missing-phase', path, stage, driver, trace=True, reject=True)
save(True)
raise SystemExit(0 if all(c['passed'] for c in checks) else 1)
