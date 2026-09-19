"""Development-only parity for context evidence and command-list inventory."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

p=argparse.ArgumentParser()
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--exe',type=Path,required=True)
p.add_argument('--captures',type=Path,required=True)
p.add_argument('--fixtures',type=Path,required=True)
p.add_argument('--qt-bin',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
args=p.parse_args();args.out.mkdir(parents=True,exist_ok=False)
sys.path.insert(0,str(args.reference.resolve()))
from frame import Frame
from contexts import describe,recovery_report,TYPES
from command_lists import inventory
from api_commands import commands
env=dict(os.environ);env['PATH']=str(args.qt_bin.resolve())+os.pathsep+env['PATH']
results=[]
def native(command,path,target,extra=()):
    run=subprocess.run([str(args.exe.resolve()),command,str(path.resolve()),'--out',str(target),*extra],env=env,capture_output=True,timeout=180)
    (args.out/(target.name+'.log')).write_bytes(run.stdout+run.stderr)
    return run
cases=[('gf2',args.captures/'GF2_Exilium_2026_03_03__00_19_35.gpa_frame'),('bf1',args.captures/'bf1_2026_01_21__16_53_05.gpa_frame')]
cases += [(p.stem,p) for p in sorted(args.fixtures.glob('*.gpa_frame'))]
for name,path in cases:
    with Frame(path) as frame:
        expected={'contexts':[],'invalid':[],'recovery':recovery_report(frame)}
        for e in frame.entries.values():
            if e.category==5 and e.type in TYPES:
                try: expected['contexts'].append(describe(frame,e.id))
                except ValueError as error: expected['invalid'].append(dict(id=e.id,error=str(error)))
        expected['contexts']+=expected['recovery']['contexts']
        target=args.out/(name+'-contexts');run=native('contexts',path,target)
        assert run.returncode==0,(name,run.stderr)
        actual=json.loads((target/'contexts.json').read_text(encoding='utf-8'))
        assert actual==expected,(name,'context report',actual,expected)
        target=args.out/(name+'-lists');run=native('command-lists',path,target)
        try: expected_lists=inventory(frame)
        except ValueError as error:
            assert run.returncode!=0,(name,'native accepted invalid inventory')
            actual_error=json.loads(run.stderr)['error'];assert actual_error==str(error),(name,actual_error,str(error))
        else:
            assert run.returncode==0,(name,run.stderr)
            assert json.loads((target/'command-lists.json').read_text(encoding='utf-8'))==expected_lists,(name,'list inventory')
        target=args.out/(name+'-commands');run=native('commands',path,target)
        assert run.returncode==0,(name,run.stderr)
        actual_commands=json.loads((target/'commands.json').read_text(encoding='utf-8'))['commands']
        expected_commands=commands(frame)
        assert actual_commands==expected_commands,(name,'API metadata')
        results.append(dict(case=name,passed=True,contexts=len(expected['contexts']),recovered=len(expected['recovery']['contexts']),issues=len(expected['recovery']['issues']),commands=len(expected_commands)))
        print(name,'PASS',flush=True)

# Execute the capture whose only context identity is inferred from a Map READ pair.
frame=args.fixtures/'replay.gpa_frame'
for resource in (7,13):
    key='replay-buffer-'+str(resource);actual=args.out/(key+'-native');reference=args.out/(key+'-reference')
    run=native('buffer',frame,actual,('--id',str(resource),'--event','117'))
    assert run.returncode==0,run.stderr
    run=subprocess.run([sys.executable,str(args.reference/'analyze.py'),str(frame),'buffer','--out',str(reference),'--id',str(resource),'--event','117','--after'],env=env,capture_output=True,timeout=180)
    (args.out/(key+'-reference.log')).write_bytes(run.stdout+run.stderr)
    assert run.returncode==0,run.stderr
    assert (actual/'buffer.bin').read_bytes()==(reference/'buffer.bin').read_bytes(),key
    results.append(dict(case=key,passed=True));print(key,'PASS',flush=True)
(args.out/'validation.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
