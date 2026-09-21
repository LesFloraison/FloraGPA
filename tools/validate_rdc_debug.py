"""Development-only full native shader trace comparison with the original Python worker."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import struct
import sys

p = argparse.ArgumentParser(description=__doc__)
for key in ('reference', 'exe', 'cli', 'qt-bin', 'captures', 'out'):
    p.add_argument('--' + key, type=Path, required=True)
a = p.parse_args()
a.out = a.out.resolve()
a.out.mkdir(parents=True, exist_ok=False)
env = {k: v for k, v in os.environ.items() if k.upper() in
       ('SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'APPDATA', 'LOCALAPPDATA')}
env['PATH'] = str(a.qt_bin.resolve()) + ';' + os.environ['WINDIR'] + '/System32;' + os.environ['WINDIR']
dll = 'C:/Program Files/RenderDoc/renderdoc.dll'
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
checks = []
sys.path[:0] = [str(a.reference / 'standalone'), str(a.reference / 'tools')]
from frame import Frame
from shader_project import compile_project, FORMAT
from validate_buffer_edits import graphics_fixture, compute_fixture
from validate_class_linkage import clone


def check(name, passed, **details):
    checks.append(dict(name=name, passed=bool(passed), **details))
    (a.out / 'validation.json').write_text(json.dumps(dict(passed=all(c['passed'] for c in checks),
                                                         checks=checks), indent=2), encoding='utf-8')
    print(name, 'PASS' if passed else 'FAIL', flush=True)
    assert passed, (name, details)


def native(label, job, valid=True):
    out = a.out / (label + '-native')
    job = dict(job, out=str(out), renderdoc=dll)
    source = a.out / (label + '-native-job.json')
    source.write_text(json.dumps(job), encoding='utf-8')
    run = subprocess.run([str(a.exe.resolve()), '--job', str(source)], env=env,
                         capture_output=True, timeout=180)
    (a.out / (label + '-native.log')).write_bytes(run.stdout + run.stderr)
    result = json.loads((out / 'result.json').read_text('utf-8'))
    check(label + '-native-status', (run.returncode == 0) == valid and result['ok'] == valid,
          error=result.get('error'))
    modules = [Path(x).name.lower() for x in result['loaded_modules']]
    check(label + '-native-runtime', not any(x.startswith(('python', 'tk8', 'tcl8', 'gpa_', 'gpa-')) or
          x in ('gpa.dll', 'dx11_player.dll', 'dx11_playback.dll', 'shimd3d64.dll') for x in modules))
    return result


def oracle(label, job):
    out = a.out / (label + '-oracle')
    out.mkdir()
    source = a.out / (label + '-oracle-job.json')
    source.write_text(json.dumps(dict(job, out=str(out))), encoding='utf-8')
    script = a.out / (label + '-oracle.py')
    script.write_text('import sys\nsys.path.insert(0,' + repr(str(a.reference / 'standalone')) + ')\n'
                      'from rdc_worker import run\nrun(' + repr(str(source)) + ')\nsys.exit(0)\n')
    run = subprocess.run(['C:/Program Files/RenderDoc/qrenderdoc.exe', '--python', str(script)],
                         startupinfo=startup, capture_output=True, timeout=180)
    (a.out / (label + '-oracle.log')).write_bytes(run.stdout + run.stderr)
    return json.loads((out / 'result.json').read_text('utf-8'))


def debug(label, capture, action, valid=True, **options):
    job = dict(action=action, capture=str(capture.resolve()), gpa_event=None, eid=None, x=0, y=0,
               sample=0, vertex=0, instance=0, index=None, group=[0, 0, 0], thread=[0, 0, 0])
    job.update(options)
    actual, want = native(label, job, valid), oracle(label, job)
    check(label + '-oracle-status', want['ok'] == valid, error=want.get('error'))
    if valid:
        # Only the documented uninitialized global offsets in RenderDoc 1.45's
        # DXBC implementation are unavailable. Keep raw oracle output on disk;
        # every instruction mapping and real constant-member offset is compared.
        want = copy.deepcopy(want)
        missing = []
        for i, mapping in enumerate(want['trace']['sourceVars']):
            invalid = mapping['signatureIndex'] >= 0 or any(
                r['type'] in (1, 6) or (r['type'] == 2 and mapping['type'] == 255 and
                                        mapping['rows'] == 0 and mapping['columns'] == 0)
                for r in mapping['variables'])
            if invalid:
                missing.append(i)
                mapping['offset'] = None
        check(label + '-offset-gaps', actual['debug_scope']['unavailable_global_source_offsets'] == missing)
        fields = ('eid', 'gpa_event', 'gpa_event_map', 'gpa_command_map', 'actual_vertex_index',
                  'trace', 'source_debug', 'disassembly', 'steps', 'step_count')
        differences = [k for k in fields if actual.get(k) != want.get(k)]
        check(label + '-trace-parity', not differences, differences=differences,
              steps=actual['step_count'], source_files=len(actual['source_debug']['files']))
        check(label + '-disassembly-export', (a.out / (label + '-native/debug_shader.asm')).read_text('utf-8') == actual['disassembly'])
    return actual


def capture(label, source, warp):
    out = a.out / (label + '-capture')
    cmd = [str(a.cli.resolve()), 'replay', str(source.resolve()), '--renderdoc', dll, '--out', str(out)]
    if warp:
        cmd.append('--warp')
    run = subprocess.run(cmd, env=env, capture_output=True, timeout=180)
    (a.out / (label + '-capture.log')).write_bytes(run.stdout + run.stderr)
    check(label + '-capture', run.returncode == 0)
    return Path(json.loads((out / 'report.json').read_text('utf-8'))['rdc_capture'])


for warp in (False, True):
    driver = 'warp' if warp else 'hardware'
    samples = a.reference / 'analysis/capture_samples'
    source = capture('source-' + driver, samples / 'shader_sources/source.gpa_frame', warp)
    mapping = native('source-' + driver + '-index', dict(action='events', capture=str(source)))['index']
    commands = list(mapping['gpa_command_map'].values())
    draw = max(c['gpa_event'] for c in commands if c['name'].startswith('Draw') and c['status'] == 'mapped')
    dispatch = next(c['gpa_event'] for c in commands if c['name'] == 'Dispatch' and c['status'] == 'mapped')
    vertex = debug('source-vs-' + driver, source, 'debug-vertex', gpa_event=draw, vertex=2)
    compute = debug('source-cs-' + driver, source, 'debug-thread', gpa_event=dispatch)
    pixel = debug('source-ps-' + driver, source, 'debug-pixel', gpa_event=draw, x=48, y=32)
    check('source-locations-' + driver, all(r['trace']['instInfo'] and r['source_debug']['sourceDebugInformation']
                                           for r in (vertex, compute, pixel)))
    project = capture('project-' + driver, samples / 'shader_project/project.gpa_frame', warp)
    multi = debug('project-ps-' + driver, project, 'debug-pixel', x=48, y=32)
    check('multi-file-source-' + driver, len(multi['source_debug']['files']) >= 3 and multi['step_count'] == 7)
    loop_text = ('float4 main(float4 pos:SV_Position):SV_Target\n{\n float value=pos.x;\n'
                 ' [loop] for(uint i=0;i<3;i++)\n {\n  value=value+1;\n }\n'
                 ' return float4(value,0,0,1);\n}\n')
    code, _ = compile_project(dict(format=FORMAT, files=[dict(name='loop.hlsl', text=loop_text)],
                                    root='loop.hlsl', entry='main', profile='ps_5_0', flags=5,
                                    defines=[], include_dirs=[]))
    original = samples / 'shader_project/project.gpa_frame'
    with Frame(original) as frame:
        data_id = frame.resource(27)['data_id']
    modified = clone(original, a.out / ('loop-' + driver + '.gpa_frame'),
                     lambda en, raw: struct.pack('<Q', len(code)) + code + bytes(8) if en.id == data_id else raw)
    loop_capture = capture('loop-' + driver, modified, warp)
    loop = debug('loop-ps-' + driver, loop_capture, 'debug-pixel', x=48, y=32)
    final = {}
    for step in loop['steps']:
        for change in step['changes']:
            if change['after']['name']:
                final[change['after']['name']] = change['after']
    check('loop-known-output-' + driver, final['o0']['value']['f32v'][:4] == [51.5, 0., 0., 1.]
          and len({s['nextInstruction'] for s in loop['steps']}) < loop['step_count'])
    if not warp:
        debug('dispatch-as-pixel', source, 'debug-pixel', valid=False, gpa_event=dispatch)
        debug('draw-as-compute', source, 'debug-thread', valid=False, gpa_event=draw)
        debug('missing-event', source, 'debug-vertex', valid=False, gpa_event=999999999)
        debug('empty-pixel', source, 'debug-pixel', valid=False, gpa_event=draw, x=10000, y=10000)

msaa = a.captures / 'msaa-rdc/independent_capture.rdc'
debug('msaa-vs', msaa, 'debug-vertex', gpa_event=33, vertex=2)
debug('msaa-explicit-index', msaa, 'debug-vertex', gpa_event=33, vertex=2, index=1)
debug('msaa-no-fragment', msaa, 'debug-pixel', valid=False, gpa_event=33, x=32, y=32)
bf1 = a.captures / 'bf1-rdc/independent_capture.rdc'
vertex = debug('bf1-indexed-vs', bf1, 'debug-vertex', gpa_event=876)
compute = debug('bf1-cs', bf1, 'debug-thread', gpa_event=209)
check('bf1-known-step-counts', vertex['step_count'] == 59 and compute['step_count'] == 53)
gf2 = a.captures / 'gf2-rdc/independent_capture.rdc'
pixel = debug('gf2-ps', gf2, 'debug-pixel', gpa_event=1508, x=1280, y=720)
check('gf2-known-step-count', pixel['step_count'] == 3)

seed = graphics_fixture(a.out / 'indexed-seed.gpa_frame')
for width, fmt in ((2, 57), (4, 42)):
    data = struct.pack('<5' + ('H' if width == 2 else 'I'), 99, 99, 1, 2, 3)

    def edit(en, raw):
        if en.id == 22:
            return raw[:16] + struct.pack('<I', len(data)) + raw[20:]
        if en.id == 23:
            return struct.pack('<I', len(data)) + data
        if en.id in (99, 199):
            raw = bytearray(raw)
            struct.pack_into('<II', raw, 136, fmt, width)
            return raw
        if en.id in (100, 200):
            return raw[:24] + struct.pack('<IIi', 3, 1, -1)
        return raw

    modified = clone(seed, a.out / ('index' + str(width) + '.gpa_frame'), edit)
    indexed = capture('index' + str(width), modified, False)
    result = debug('index' + str(width), indexed, 'debug-vertex', gpa_event=100, vertex=1)
    check('index' + str(width) + '-known-fetch', result['actual_vertex_index'] == 1)
    debug('index' + str(width) + '-explicit', indexed, 'debug-vertex', gpa_event=100, vertex=1, index=2)
    debug('index' + str(width) + '-outside', indexed, 'debug-vertex', valid=False, gpa_event=100, vertex=10000)

for kind in ('typed', 'structured'):
    source = compute_fixture(a.out / ('compute-' + kind + '.gpa_frame'), kind=kind)
    compute = capture('compute-' + kind, source, False)
    debug('compute-' + kind, compute, 'debug-thread', gpa_event=100, thread=[3, 0, 0])
print('All native shader trace comparisons passed', flush=True)
