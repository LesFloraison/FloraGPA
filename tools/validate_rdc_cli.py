"""Compare the public native RDC CLI with the original Python CLI, serially."""
import argparse
import copy
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import shutil

p = argparse.ArgumentParser(description=__doc__)
for key in ('reference','cli','qt-bin','captures','debug','mesh','out'):
    p.add_argument('--' + key, type=Path, required=True)
a = p.parse_args()
a.out = a.out.resolve()
a.out.mkdir(parents=True, exist_ok=False)
env = {k:v for k,v in os.environ.items() if k.upper() in
       ('SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','APPDATA','LOCALAPPDATA')}
env['PATH'] = str(a.qt_bin.resolve()) + ';C:/Windows/System32;C:/Windows'
checks = []
count = 0

def save(complete=False):
    (a.out/'validation.json').write_text(json.dumps(dict(
        complete=complete, passed=complete and all(c['passed'] for c in checks),
        comparisons=count, checks=checks), indent=2), 'utf-8')

def check(name, ok, **detail):
    checks.append(dict(name=name, passed=bool(ok), **detail))
    save()
    print(name, 'PASS' if ok else 'FAIL', flush=True)
    assert ok, name

def compare(label, capture, action, **options):
    global count
    native = a.out/(label+'-native')
    oracle = a.out/(label+'-oracle')
    args = []
    for key, value in options.items():
        args.append('--'+key.replace('_','-'))
        args.extend(map(str,value if isinstance(value,list) else [value]))
    command = [str(a.cli.resolve()), 'rdc-analyze', str(capture.resolve()), action,
               '--out', str(native), '--renderdoc', 'C:/Program Files/RenderDoc/renderdoc.dll', *args]
    run = subprocess.run(command, env=env, capture_output=True, timeout=200)
    (a.out/(label+'-native.log')).write_bytes(run.stdout+run.stderr)
    check(label+'-native', run.returncode==0, error=run.stderr.decode('utf-8','replace'))
    check(label+'-printed-result', Path(run.stdout.decode('utf-8').strip()).resolve()==(native/'result.json').resolve())
    run = subprocess.run([sys.executable, str(a.reference/'standalone/rdc_analyze.py'),
                          str(capture.resolve()), action, '--out', str(oracle), *args],
                         capture_output=True, timeout=200)
    (a.out/(label+'-oracle.log')).write_bytes(run.stdout+run.stderr)
    check(label+'-oracle', run.returncode==0, error=run.stderr.decode('utf-8','replace'))
    nj = json.loads((native/'job.json').read_text('utf-8'))
    oj = json.loads((oracle/'job.json').read_text('utf-8'))
    for job in (nj,oj):
        job.pop('out')
        job.pop('renderdoc', None)
        job['capture'] = str(Path(job['capture']).resolve())
    check(label+'-prepared-job', nj==oj)
    actual = json.loads((native/'result.json').read_text('utf-8'))
    expected = json.loads((oracle/'result.json').read_text('utf-8'))
    check(label+'-reports-ok', actual['ok'] and expected['ok'])
    common = ('eid','gpa_event','gpa_event_map','gpa_command_map')
    fields = {
        'inventory': ('actions','resources','textures','buffers'),
        'postmesh': ('stage','mesh','vertex_count','index_count','face_count'),
        'history': ('resource','resource_name','x','y','history','history_events','history_scope'),
        'texture': ('resource','resource_name','byte_length','subresource'),
        'counters': ('available','result_count','note','values'),
    }.get(action, ('actual_vertex_index','trace','source_debug','disassembly','steps','step_count'))
    left, right = copy.deepcopy(actual), copy.deepcopy(expected)
    if action.startswith('debug-'):
        missing = []
        for i,mapping in enumerate(right['trace']['sourceVars']):
            invalid = mapping['signatureIndex']>=0 or any(
                r['type'] in (1,6) or (r['type']==2 and mapping['type']==255 and
                                      mapping['rows']==0 and mapping['columns']==0)
                for r in mapping['variables'])
            if invalid:
                missing.append(i)
                mapping['offset']=None
        check(label+'-documented-offsets', left['debug_scope']['unavailable_global_source_offsets']==missing)
    if action=='counters':
        timings=0
        for report in (left,right):
            for row in report['values']:
                if row['unit']=='CounterUnit.Seconds':
                    assert isinstance(row['value'],(int,float)) and math.isfinite(row['value']) and row['value']>=0
                    row['value']='<independently measured duration>'
                    timings+=1
        check(label+'-valid-durations', timings>0, independent_measurements=timings)
    differences = [k for k in (*common,*fields) if left.get(k)!=right.get(k)]
    check(label+'-full-result-parity', not differences, differences=differences)
    check(label+'-module-sidecar', json.loads((native/'loaded_modules.json').read_text('utf-8'))==actual['loaded_modules']
          and actual['gpa_modules_loaded']==expected['gpa_modules_loaded']==[]
          and isinstance(json.loads((oracle/'loaded_modules.json').read_text('utf-8')),list))
    names = sorted(f.name for f in oracle.iterdir() if f.name not in
                   ('job.json','run_job.py','result.json','worker.log','loaded_modules.json'))
    actual_names = sorted(f.name for f in native.iterdir() if f.name not in ('job.json','result.json','worker.log','loaded_modules.json'))
    check(label+'-artifact-names', actual_names==names)
    for name in names:
        check(label+'-'+name, (native/name).read_bytes()==(oracle/name).read_bytes())
    check(label+'-native-log', (native/'worker.log').is_file() and not (native/'run_job.py').exists())
    modules = [Path(s).name.lower() for s in actual['loaded_modules']]
    check(label+'-runtime', not any(m.startswith(('python','tk8','tcl8','gpa_','gpa-')) or
          m in ('gpa.dll','dx11_player.dll','dx11_playback.dll','shimd3d64.dll') for m in modules))
    count += 1
    save()

source = a.debug/'source-hardware-capture/independent_capture.rdc'
warp = a.debug/'source-warp-capture/independent_capture.rdc'
msaa = a.captures/'msaa-rdc/independent_capture.rdc'
commands = a.captures/'commands-rdc/independent_capture.rdc'
compare('inventory-default', source, 'inventory')
compare('postmesh', source, 'postmesh', gpa_event=105, stage='VSOut', instance=0)
compare('history', msaa, 'history', gpa_event=33, x=32, y=32, sample=1)
compare('texture-subresource', commands, 'texture', gpa_event=300, resource=84, mip=1, layer=1)
compare('counters', source, 'counters')
compare('pixel', source, 'debug-pixel', gpa_event=105, x=48, y=32)
compare('vertex', source, 'debug-vertex', gpa_event=105, vertex=2)
compare('thread', source, 'debug-thread', gpa_event=51, group=[0,0,0], thread=[0,0,0])
compare('thread-warp', warp, 'debug-thread', gpa_event=51, group=[0,0,0], thread=[1,0,0])
compare('history-native-event', msaa, 'history', eid=47, x=8, y=8)
compare('explicit-index', msaa, 'debug-vertex', gpa_event=33, vertex=2, index=1)
compare('gs-stage', a.mesh/'gs-capture/independent_capture.rdc', 'postmesh', stage='GSOut')
compare('gs-instance', a.mesh/'instances-hardware-capture/independent_capture.rdc', 'postmesh',
        gpa_event=100, stage='GSOut', instance=1)
compare('fractional-timeout', source, 'inventory', timeout=30.5)
unicode_capture = a.out/'capture 测试 & input.rdc'
shutil.copyfile(source, unicode_capture)
compare('unicode 测试 & output', unicode_capture, 'inventory', gpa_event=105)
save(complete=True)
