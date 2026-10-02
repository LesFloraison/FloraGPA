"""Serial, development-only compatibility measurements. Never a production backend."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import time
from catalog_captures import digest

def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

def run(command, log, timeout, env):
    started = time.monotonic()
    with log.open('wb') as stream:
        try:
            process = subprocess.Popen([str(x) for x in command], stdout=stream, stderr=subprocess.STDOUT, env=env)
        except OSError as error:
            stream.write(str(error).encode('utf-8'))
            return {'exit_code': None, 'timed_out': False, 'seconds': round(time.monotonic()-started, 3),
                    'log': log.name, 'launch_error': str(error)}
        expired = False
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            expired = True
            if os.name == 'nt':
                subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], stdout=stream, stderr=subprocess.STDOUT)
            process.kill()
            code = process.wait()
    return {'exit_code': code, 'timed_out': expired, 'seconds': round(time.monotonic()-started, 3),
            'log': log.name}

def load(path):
    return json.loads(path.read_text(encoding='utf-8')) if path.is_file() else None


def original_kernel_completed(report):
    # A wrapper can fail to name/decode multi-swap-chain exports after successful playback.
    # This does not certify its images, adapter selection, or cross-player equivalence.
    return (report.get('open_status') == 0 and report.get('playback_status') == 0 and
            report.get('closed') is True and report.get('callbacks') == [])

def image_difference(a, b):
    if len(a) != len(b):
        return {'same_byte_length': False, 'byte_equal': False}
    changed, total, maximum = 0, 0, 0
    for x, y in zip(a, b):
        d = abs(x-y); changed += d != 0; total += d; maximum = max(maximum, d)
    return {'byte_equal': not changed, 'bytes': len(a), 'changed_bytes': changed,
            'mean_absolute_rgba': total/len(a) if a else 0, 'max_absolute_channel': maximum}

def original_rgba(directory):
    files=sorted(directory.glob('framebuffer_new*.tga'))
    if len(files)!=1: return None
    data=files[0].read_bytes()
    if len(data)<18: raise ValueError('Truncated original TGA header')
    ident,cmap,kind=data[:3]
    w,h,bits,flags=struct.unpack_from('<HHBB',data,12)
    if cmap or kind!=2 or bits!=32 or flags&0x10: raise ValueError('Unsupported original TGA export')
    raw=data[18+ident:18+ident+w*h*4]
    if len(raw)!=w*h*4:raise ValueError('Truncated original TGA export')
    if not flags&0x20:raw=b''.join(raw[y*w*4:(y+1)*w*4] for y in reversed(range(h)))
    rgba=bytearray(raw);rgba[0::4],rgba[2::4]=raw[2::4],raw[0::4]
    return bytes(rgba)

def summarize_native(runs, case):
    good = all(r.get('report', {}).get('completed') and r['exit_code'] == 0 for r in runs)
    if not good: return 'replay_failed'
    hashes = [r['report'].get('rgba_sha256') for r in runs]
    expected = case.get('reference_rgba_sha256')
    if expected and any(h != expected for h in hashes): return 'golden_mismatch'
    if all(h is None for h in hashes): return 'replayed_without_image'
    return 'repeat_stable' if len(set(hashes)) == 1 else 'repeat_variable'

def prioritize(finding, cases):
    affected = [c for c in cases if c['id'] in finding['captures']]
    blocking = finding['severity'] == 'error'
    if blocking and any(c['origin']=='user_supplied_game_capture' for c in affected):
        priority, basis = 0, 'real game capture blocker'
    elif finding.get('python_evidence'):
        priority, basis = 1, 'linked recovered Python implementation'
    elif len(affected)>1 or finding['kind']=='auxiliary_audit':
        priority, basis = 2, 'shared semantics or coverage gap'
    else:
        priority, basis = 3, 'isolated or insufficiently evidenced path'
    return dict(finding, priority=priority, priority_basis=basis)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--manifest', type=Path, required=True)
    p.add_argument('--captures-root', type=Path, required=True)
    p.add_argument('--exe', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--oracle-tools', type=Path, help='Original research tools directory containing replay_frame.py; optional, explicitly recorded')
    p.add_argument('--case', action='append', default=[], help='Case ID, may be repeated')
    p.add_argument('--repeat', type=int, default=2)
    p.add_argument('--timeout', type=int, default=180)
    p.add_argument('--preflight-only', action='store_true')
    args = p.parse_args()
    if args.repeat < 2 or args.timeout <= 5: p.error('repeat must be >=2 and timeout >5 seconds')
    manifest = load(args.manifest)
    cases = manifest['cases']
    if len({c['id'] for c in cases}) != len(cases): p.error('duplicate case ID')
    if set(args.case) - {c['id'] for c in cases}: p.error('unknown case ID')
    cases = [c for c in cases if not args.case or c['id'] in args.case]
    args.out.mkdir(parents=True, exist_ok=False)
    root, exe = args.captures_root.resolve(), args.exe.resolve(strict=True)
    env = {k:v for k,v in os.environ.items() if k.upper() in {'SYSTEMROOT','WINDIR','COMSPEC','TEMP','TMP','LOCALAPPDATA','APPDATA'}}
    windows = Path(os.environ['WINDIR'])
    env['PATH'] = str(windows/'System32') + os.pathsep + str(windows)
    oracle_env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    summary = {'schema':'FloraGPA compatibility run 1', 'exe_sha256':digest(exe),
               'manifest_sha256':digest(args.manifest), 'profile':manifest['profile'],
               'original_kernel_mode':'private ABI adapter, single playback, default adapter not identified',
               'cross_implementation_equivalence':'not_asserted_without_matched_device_and_configuration',
               'cases':[], 'queue':[], 'coverage':[]}
    try:
        drivers = subprocess.run(['powershell','-NoProfile','-Command',
            'Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,PNPDeviceID | ConvertTo-Json'],
            capture_output=True, timeout=30)
        summary['installed_video_controllers'] = json.loads(drivers.stdout.decode('utf-8-sig'))
    except Exception as error: summary['device_inventory_error'] = str(error)
    coverage, queue = {}, {}
    for case in cases:
        case_id = case['id']
        if Path(case_id).name != case_id or case_id in {'.','..'}: raise ValueError('unsafe case ID')
        directory = args.out/case_id; directory.mkdir()
        item = {'id':case_id,'family':case['family'],'policy':case['comparison_policy'],'native':[],'original':[]}
        summary['cases'].append(item)
        capture = (root/case['path']).resolve()
        if not capture.is_relative_to(root) or not capture.is_file() or digest(capture) != case['sha256']:
            item['status']='capture_missing_or_hash_mismatch'
            write(args.out/'validation.json',summary)
            print(case_id,item['status'],flush=True); continue
        check = run([exe,'validate-frame',capture,'--out',directory/'preflight'],directory/'preflight.log',args.timeout,env)
        check['report'] = load(directory/'preflight/validation.json')
        item['preflight'] = check
        validation = check['report'] or {}
        for row in validation.get('coverage',[]):
            key=(row['category'],row['record_type'])
            if key not in coverage: coverage[key]=dict(row,captures=[])
            else:
                for field in ['count','decoded_records','unchecked_records']: coverage[key][field]+=row[field]
            coverage[key]['captures'].append(case_id)
        for finding in validation.get('findings',[]):
            key=(finding['kind'],finding['category'],finding['record_type'])
            if key not in queue: queue[key]=dict(finding,captures=[],occurrences=0)
            queue[key]['occurrences']+=1
            if case_id not in queue[key]['captures']:queue[key]['captures'].append(case_id)
        if not args.preflight_only:
            for i in range(args.repeat):
                output=directory/f'native-{i+1}'
                r=run([exe,'replay',capture,'--out',output],directory/f'native-{i+1}.log',args.timeout,env)
                r['report']=load(output/'report.json') or {}
                r['runtime_dependency_audit']='not_available'
                if r['report'].get('loaded_modules'):
                    forbidden=[s for s in r['report']['loaded_modules'] if Path(s).name.lower().startswith(('python','tk8','tcl8','gpa-','gpa_')) or
                               Path(s).name.lower() in {'gpa.dll','dx11_player.dll','dx11_playback.dll','shimd3d64.dll','renderdoc.dll'}]
                    r['runtime_dependency_audit']='failed' if forbidden else 'passed'
                    r['forbidden_modules']=forbidden
                r['artifacts']={f.name:digest(f) for f in output.glob('*') if f.is_file() and f.suffix in {'.rgba','.bin','.png'}}
                item['native'].append(r)
            item['status']=summarize_native(item['native'],case)
            raw=[directory/f'native-{i+1}/frame.rgba' for i in range(args.repeat)]
            if all(x.exists() for x in raw):item['native_repeat_difference']=image_difference(raw[0].read_bytes(),raw[1].read_bytes())
            if args.oracle_tools:
                for i in range(args.repeat):
                    output=directory/f'original-{i+1}'
                    r=run([sys.executable,args.oracle_tools.resolve()/'replay_frame.py',capture,'--out',output,'--timeout',args.timeout-5],
                          directory/f'original-{i+1}.log',args.timeout,oracle_env)
                    r['report']=load(output/'replay_report.json') or {}
                    r['kernel_report']=load(output/'native_result.json') or {}
                    r['kernel_completed']=original_kernel_completed(r['kernel_report'])
                    r['exports']={f.name:digest(f) for f in output.glob('framebuffer*')
                                  if f.is_file() and f.suffix in {'.tga','.dds'}}
                    item['original'].append(r)
                if all(r['exit_code']==0 and r['report'].get('native',{}).get('closed') for r in item['original']):
                    item['original_status']='repeat_stable' if len({r['report']['replayed_rgba_sha256'] for r in item['original']})==1 else 'repeat_variable'
                    original=[original_rgba(directory/f'original-{i+1}') for i in range(args.repeat)]
                    if all(x is not None for x in original):
                        item['original_repeat_difference']=image_difference(original[0],original[1])
                        if raw[0].exists():item['native_original_observed_difference']=image_difference(raw[0].read_bytes(),original[0])
                else:
                    item['original_status']=('export_adapter_failed' if
                        all(r['kernel_completed'] for r in item['original']) else 'replay_or_export_failed')
            else:item['original_status']='not_run'
            controls=[]
            for control in case.get('controls',[]):
                outcomes=[]
                for i in range(control['repeat']):
                    output=directory/f"control-{control['disable_event']}-{i+1}"
                    r=run([exe,'replay',capture,'--disable',control['disable_event'],'--out',output],
                          directory/f"control-{control['disable_event']}-{i+1}.log",args.timeout,env)
                    r['report']=load(output/'report.json') or {};outcomes.append(r)
                controls.append({'diagnostic_only':True,'disabled_event':control['disable_event'],'runs':outcomes,
                    'passed':all(r['exit_code']==0 and r['report'].get('rgba_sha256')==control['expected_rgba_sha256'] for r in outcomes)})
            item['controls']=controls
            item['boundaries']=[]
            for boundary in case.get('boundaries',[]):
                results=[]; blobs=[]
                for i in range(args.repeat):
                    tag=f"boundary-{boundary['resource']}-{boundary['event']}-{i+1}"
                    output=directory/tag
                    r=run([exe,'texture-storage',capture,'--id',boundary['resource'],'--event',boundary['event'],'--out',output],
                          directory/(tag+'.log'),args.timeout,env)
                    r['report']=load(output/'report.json') or {}; results.append(r)
                    if (output/'texture.bin').exists():blobs.append((output/'texture.bin').read_bytes())
                complete=len(blobs)==args.repeat and all(r['exit_code']==0 and r['report'].get('completed') for r in results)
                stable=complete and len({hashlib.sha256(b).hexdigest() for b in blobs})==1
                item['boundaries'].append(dict(boundary,runs=results,complete=complete,stable=stable,
                    passed=complete and (stable or not boundary['expect_stable']),
                    difference=image_difference(blobs[0],blobs[1]) if complete else None))
        else:item['status']='preflight_only'
        # Runtime failures can expose semantics outside offline coverage.
        if item['status'] in {'replay_failed','golden_mismatch'}:
            queue[('runtime',case_id,0)]={'kind':item['status'],'severity':'error','reason':'See native worker logs and retained reports',
                'captures':[case_id],'occurrences':1,'event_id':None,'resource_id':None,'record_type':None}
        summary['coverage']=list(coverage.values())
        summary['queue']=sorted((prioritize(f,cases) for f in queue.values()),
            key=lambda f:(f['priority'],f['severity']!='error',-len(f['captures']),str(f['kind'])))
        write(args.out/'validation.json',summary)
        print(case_id,item['status'],item.get('original_status',''),flush=True)
    summary['counts']={}
    for item in summary['cases']:summary['counts'][item['status']]=summary['counts'].get(item['status'],0)+1
    write(args.out/'validation.json',summary)
    write(args.out/'coverage.json',summary['coverage']);write(args.out/'repair-queue.json',summary['queue'])
    return int(any(c['status'] in {'capture_missing_or_hash_mismatch','golden_mismatch'} or
                   not c.get('preflight',{}).get('report') or
                   any(x.get('runtime_dependency_audit')=='failed' for x in c.get('native',[])) or
                   any(not x['passed'] for x in c.get('boundaries',[])) or
                   any(not x['passed'] for x in c.get('controls',[])) for c in summary['cases']))

if __name__=='__main__':sys.exit(main())
