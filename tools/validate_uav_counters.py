"""Development-only counter/readback parity against the preserved Python replay."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

p=argparse.ArgumentParser()
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--exe',type=Path,required=True)
p.add_argument('--captures',type=Path,required=True)
p.add_argument('--fixture',type=Path,required=True)
p.add_argument('--qt-bin',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
args=p.parse_args()
args.out.mkdir(parents=True,exist_ok=False)
env=dict(os.environ)
env['PATH']=str(args.qt_bin.resolve())+os.pathsep+env['PATH']
results=[]
def run(cmd,log):
    result=subprocess.run(cmd,env=env,capture_output=True,timeout=180)
    log.write_bytes(result.stdout+result.stderr)
    if result.returncode:raise RuntimeError(f'{log.name}: {result.stderr.decode(errors="replace")[-2000:]}')

def compare(key,frame,event,resource,project,after=False,append_start=None):
    path=args.out/(key+'.json')
    path.write_text(json.dumps(project),encoding='utf-8')
    shared=['--id',str(resource),'--event',str(event),'--experiment',str(path)]
    native,reference=(args.out/(key+'-'+which) for which in ('native','reference'))
    run([str(args.exe.resolve()),'buffer',str(frame),'--out',str(native),*shared,*([] if after else ['--before'])],args.out/(key+'-native.log'))
    run([sys.executable,str(args.reference/'analyze.py'),str(frame),'buffer','--out',str(reference),*shared,*(['--after'] if after else [])],args.out/(key+'-reference.log'))
    actual=json.loads((native/'report.json').read_text())['uav_counters']
    expected=json.loads((reference/'result.json').read_text())['uav_counters']
    assert actual==expected,(key,actual,expected)
    a,b=(native/'buffer.bin').read_bytes(),(reference/'buffer.bin').read_bytes()
    if append_start is None:
        assert a==b,(key,'buffer differs')
    else:
        # This BF1 dispatch emits packed tile IDs through parallel Append calls. The native replay
        # itself changes their order across runs; compare every element with its multiplicity.
        assert after and len(actual)==1 and actual[0]['flags']==2
        info=actual[0]; stride=info['stride']; count=info['value']
        assert 0<=append_start<=count<=info['num_elements']
        start=(info['first_element']+append_start)*stride
        end=(info['first_element']+count)*stride
        assert len(a)==len(b) and a[:start]==b[:start] and a[end:]==b[end:],(key,'non-appended storage differs')
        assert Counter(a[i:i+stride] for i in range(start,end,stride))==Counter(b[i:i+stride] for i in range(start,end,stride)),(key,'appended payload differs')
    results.append(dict(case=key,passed=True,counters=actual,comparison='exact_bytes' if append_start is None else 'append_multiset_and_exact_unwritten_bytes',
                        raw_bytes_equal=a==b,sha256=hashlib.sha256(a).hexdigest()))
    print(key,'PASS',flush=True)
    return actual

for kind in ('append','counter'):
    frame=args.fixture/(kind+'.gpa_frame')
    project=json.loads((args.fixture/(kind+'-project.json')).read_text())
    # Seed UINT_MAX and an alias counter, then edit the selected dispatch to UINT_MAX.
    project['history']=project['history'][:3]
    project['cursor']=3
    assert compare(kind+'-initial',frame,90,10,project)[0]['value']==0xffffffff
    assert compare(kind+'-reset',frame,90,10,project,True)[0]['value']==0
    assert compare(kind+'-before',frame,100,10,project)[0]['value']==0xffffffff
    assert compare(kind+'-after',frame,100,10,project,True)[0]['value']==0
    assert compare(kind+'-next',frame,110,10,project,True)[0]['value']==1
    assert compare(kind+'-copy',frame,111,10,project,True)[0]['value']==1
    compare(kind+'-destination',frame,111,13,project,True)
    project['history'].append(dict(label='Disable first dispatch',operations=[dict(kind='enabled',event=100,value=False)]))
    project['cursor']=4
    assert compare(kind+'-disabled',frame,110,10,project,True)[0]['value']==1
    project['cursor']=2
    assert compare(kind+'-undo',frame,110,10,project,True)[0]['value']==2

for kind in ('om-append','om-counter'):
    frame=args.fixture/(kind+'.gpa_frame')
    project=json.loads((args.fixture/(kind+'-project.json')).read_text())
    assert compare(kind+'-initial',frame,190,10,project)[0]['value']==11
    assert compare(kind+'-reset',frame,190,10,project,True)[0]['value']==0
    assert compare(kind+'-before',frame,200,10,project)[0]['value']==0xffffffff
    assert compare(kind+'-after',frame,200,10,project,True)[0]['value']==0

frame=args.captures/'bf1_2026_01_21__16_53_05.gpa_frame'
project=dict(format='FloraGPA experiment 1',frame_sha256=hashlib.sha256(frame.read_bytes()).hexdigest(),frame_name=frame.name,cursor=0,
             history=[dict(label='Initial counter',operations=[dict(kind='initial_uav_counter',view=25732,value=17)]),
                      dict(label='Event counter',operations=[dict(kind='uav_counter',event=25784,view=25732,value=3)])])
baseline=compare('bf1-original-before',frame,25784,25733,project)
baseline_after=compare('bf1-original-after',frame,25784,25733,project,True,baseline[0]['value'])
project['cursor']=1
assert compare('bf1-initial-reset',frame,25784,25733,project)==baseline
project['cursor']=2
assert compare('bf1-edit-before',frame,25784,25733,project)[0]['value']==3
after=compare('bf1-edit-after',frame,25784,25733,project,True,3)
assert after[0]['value']==baseline_after[0]['value']+3-baseline[0]['value']
project['cursor']=0
assert compare('bf1-undo',frame,25784,25733,project,True,baseline[0]['value'])==baseline_after
(args.out/'validation.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
