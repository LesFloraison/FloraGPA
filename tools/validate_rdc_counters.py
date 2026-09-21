"""Compare native replay counters with the original Python worker (development only)."""
import argparse
import copy
import json
import math
import os
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
for key in ('reference','exe','qt-bin','captures','out'):
    p.add_argument('--'+key,type=Path,required=True)
p.add_argument('--cases', nargs='+', default=['commands','commands-warp','msaa','msaa-warp','msaa-disabled','msaa-before','gf2','bf1','selected-draw'])
p.add_argument('--bf1-repeat-evidence', type=Path,
               help='A previous complete BF1 native/oracle run; audit repeat variability separately from exact parity')
a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=False)
env=dict(os.environ);env['PATH']=str(a.qt_bin.resolve())+';C:/Windows/System32;C:/Windows'
checks=[];totals=dict(comparisons=0,records=0,exact_values=0,timings=0,variable_ps_values=0)
startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0

def check(name,ok,**details):
    checks.append(dict(name=name,passed=bool(ok),**details))
    complete=totals['comparisons']==len(a.cases)
    (a.out/'validation.json').write_text(json.dumps(dict(complete=complete,passed=complete and all(c['passed'] for c in checks),**totals,checks=checks),indent=2),'utf-8')
    print(name,'PASS' if ok else 'FAIL',flush=True)
    assert ok,name

def bf1_variation(actual, expected):
    """Never label independently varying PS counts as numerically equal.

    Both implementations must demonstrate the variation against prior runs of
    this exact capture; every other non-duration field remains an exact check.
    The raw four-way differences are preserved for review.
    """
    reports=[actual,expected]
    for implementation in ('native','oracle'):
        path=a.bf1_repeat_evidence/('bf1-'+implementation)/'result.json'
        reports.append(json.loads(path.read_text('utf-8')))
    previous_job=json.loads((a.bf1_repeat_evidence/'bf1-oracle-job.json').read_text('utf-8'))
    current_job=json.loads((a.out/'bf1-native-job.json').read_text('utf-8'))
    check('bf1-repeat-capture',Path(previous_job['capture']).resolve()==Path(current_job['capture']).resolve()
          and all(reports[2].get(k)==actual.get(k) for k in ('backend_version','backend_commit')))
    for i,report in enumerate(reports):
        check('bf1-repeat-identity-'+str(i), all(report.get(k)==actual.get(k) for k in
              ('eid','gpa_event','gpa_event_map','gpa_command_map','available','result_count')))
    normalized=[];ps=[]
    for report in reports:
        rows=copy.deepcopy(report['values']);counts={}
        for row in rows:
            if row['unit']=='CounterUnit.Seconds':
                assert isinstance(row['value'],(int,float)) and math.isfinite(row['value']) and row['value']>=0
                row['value']='<independently measured duration>'
            elif row['counter']==12:
                assert row['name']=='PS Invocations' and row['unit']=='CounterUnit.Absolute'
                assert type(row['value']) is int and 0<=row['value']<=2**64-1
                counts[row['eventId']]=row['value']
                row['value']='<independently measured BF1 PS invocations>'
        normalized.append(rows);ps.append(counts)
    check('bf1-repeat-stable-fields',all(rows==normalized[0] for rows in normalized))
    changed={label:sum(ps[i][k]!=ps[i+2][k] for k in ps[i])
             for i,label in enumerate(('native','oracle'))}
    check('bf1-repeat-variation-observed',all(changed.values()),changed=changed)
    details=[dict(eventId=k,native=ps[0][k],oracle=ps[1][k],previous_native=ps[2][k],previous_oracle=ps[3][k])
             for k in ps[0] if len({row[k] for row in ps})>1]
    (a.out/'bf1-ps-variation.json').write_text(json.dumps(dict(
        exact_ps_parity_established=False,previous_evidence=str(a.bf1_repeat_evidence.resolve()),
        changed=changed,values=details),indent=2),'utf-8')
    return len(ps[0])

def compare(label,capture,**selection):
    options=dict(action='counters',capture=str(capture.resolve()),gpa_event=None,eid=None,resource=None,x=0,y=0,mip=0,layer=0,sample=0,instance=0,vertex=0,index=None,stage='VSOut',group=[0,0,0],thread=[0,0,0]);options.update(selection)
    native_out=a.out/(label+'-native');job=a.out/(label+'-native-job.json')
    job.write_text(json.dumps(dict(options,out=str(native_out),renderdoc='C:/Program Files/RenderDoc/renderdoc.dll')),'utf-8')
    r=subprocess.run([str(a.exe.resolve()),'--job',str(job)],env=env,capture_output=True,timeout=180)
    (a.out/(label+'-native.log')).write_bytes(r.stdout+r.stderr)
    actual=json.loads((native_out/'result.json').read_text('utf-8'));check(label+'-native',r.returncode==0 and actual['ok'],error=actual.get('error'))
    oracle_out=a.out/(label+'-oracle');oracle_out.mkdir();oracle_job=a.out/(label+'-oracle-job.json');oracle_job.write_text(json.dumps(dict(options,out=str(oracle_out))),'utf-8')
    script=a.out/(label+'-oracle.py');script.write_text('import sys\nsys.path.insert(0,'+repr(str(a.reference/'standalone'))+')\nfrom rdc_worker import run\nrun('+repr(str(oracle_job))+')\nsys.exit(0)\n','utf-8')
    r=subprocess.run(['C:/Program Files/RenderDoc/qrenderdoc.exe','--python',str(script)],startupinfo=startup,capture_output=True,timeout=180)
    (a.out/(label+'-oracle.log')).write_bytes(r.stdout+r.stderr)
    expected=json.loads((oracle_out/'result.json').read_text('utf-8'));check(label+'-oracle',r.returncode==0 and expected['ok'],error=expected.get('error'))
    differences=[field for field in ('eid','gpa_event','gpa_event_map','gpa_command_map','available','result_count','note') if actual.get(field)!=expected.get(field)]
    check(label+'-metadata',not differences,differences=differences)
    av=copy.deepcopy(actual['values']);ev=copy.deepcopy(expected['values']);check(label+'-rows',len(av)==len(ev))
    timings=0;invalid_timings=[]
    for row in av+ev:
        if row['unit']=='CounterUnit.Seconds':
            if not (isinstance(row['value'],(int,float)) and math.isfinite(row['value']) and row['value']>=0):invalid_timings.append(row)
            row['value']='<independently measured duration>';timings+=1
    check(label+'-durations',not invalid_timings,measurements=timings,invalid=invalid_timings[:4])
    differences=[dict(native=x,oracle=y) for x,y in zip(av,ev) if x!=y]
    variable_ps=0
    if label=='bf1' and a.bf1_repeat_evidence:
        variable_ps=bf1_variation(actual,expected)
        check(label+'-stable-values-and-provenance',all(
            d['native']['counter']==12 and d['oracle']['counter']==12 and
            {k:v for k,v in d['native'].items() if k!='value'}=={k:v for k,v in d['oracle'].items() if k!='value'}
            for d in differences),difference_count=len(differences),exact_ps_parity_established=False)
    else:
        check(label+'-exact-values-and-provenance',not differences,difference_count=len(differences),differences=differences[:32])
    check(label+'-generic-selection',all(v['counter']<1000000 for v in actual['values']))
    mods=[Path(x).name.lower() for x in actual['loaded_modules']]
    check(label+'-runtime',not any(m.startswith(('python','gpa_','gpa-','tk8','tcl8')) or m in ('gpa.dll','dx11_player.dll','dx11_playback.dll','shimd3d64.dll') for m in mods))
    totals['comparisons']+=1;totals['records']+=len(av);totals['timings']+=timings//2;totals['exact_values']+=len(av)-timings//2-variable_ps
    totals['variable_ps_values']+=variable_ps
    check(label+'-complete',True)

for name in a.cases:
    if name=='selected-draw':
        compare(name,a.captures/'msaa-rdc/independent_capture.rdc',gpa_event=33)
    else:
        compare(name,a.captures/(name+'-rdc')/'independent_capture.rdc')
