"""Development oracle for coverage markers, array selection and UAV relocation."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--reference',type=Path,required=True)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--qt-bin',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();args.out=args.out.resolve();args.out.mkdir(parents=True,exist_ok=False)
    sys.path[:0]=[str(args.reference/'standalone'),str(args.reference/'tools')]
    import shaders,dxbc_patch,dxbc_uav
    from frame import Frame
    jobs=[];wanted=[];library={}
    def add(raw,action,**options):
        job=dict(input=base64.b64encode(raw).decode('ascii'),action=action,**options)
        jobs.append(job)
        try:
            metadata={}
            if action=='add':output=dxbc_patch.coverage_shader(raw,options['slot'])
            elif action=='replace':output=dxbc_patch.replacement_coverage_shader(raw,**options)
            elif action=='relocate':
                output,declared=dxbc_uav.relocate(raw,dict(options['mapping']),options.get('slot_count',8));metadata['declared']=declared
            else:
                output,mapping=dxbc_uav.reserve_rt0(raw,options['bound'],options.get('slot_count',8));metadata['mapping']={str(k):v for k,v in mapping.items()}
            wanted.append(dict(output=base64.b64encode(output).decode('ascii'),**metadata))
        except (ValueError,KeyError,IndexError,struct.error):wanted.append(dict(error=True))
    sources=[
        'float4 main():SV_Target{return float4(.2,.3,.4,.5);}',
        'float4 main(float4 p:SV_Position):SV_Target{if(p.x<4)discard;if(p.y<2)return .3;return .8;}',
        'struct O{float4 c:SV_Target;float d:SV_Depth;};O main(float4 p:SV_Position){O o;o.c=.5;o.d=p.x<4?.2:.8;return o;}',
        'float4 main(uint layer:SV_RenderTargetArrayIndex):SV_Target{return float4(layer,0,0,.5);}',
        'void main(){}',
        'RWBuffer<uint> v:register(u0);void main(float4 p:SV_Position){v[uint(p.x)]=0x0011e000u;}',
        'RWByteAddressBuffer v:register(u0);void main(){uint x;v.InterlockedAdd(0,0x0011e000u,x);}',
        'RWStructuredBuffer<uint> v:register(u7);void main(){v.IncrementCounter();v[0]=2;}',
        'RWBuffer<uint> a:register(u0);RWBuffer<uint> b:register(u7);void main(){a[0]=b[0]+1;}',
    ]
    for n,source in enumerate(sources):
        profiles=['ps_5_0'] if n>=3 else ['ps_4_0','ps_4_1','ps_5_0']
        for profile in profiles:
            raw=shaders.compile_hlsl(source,profile)[0];library[hashlib.sha256(raw).hexdigest()]=raw
            if profile=='ps_5_0':
                for slot in [0,1,7]:
                    for index in [None,0,1,0xffffffff]:
                        for routed in [False,True]:
                            for source_layout in [None,dict(register=1,mask=4,component_type=1),dict(register=0,mask=1,component_type=1)]:
                                add(raw,'replace',slot=slot,array_index=index,array_routed=routed,array_source=source_layout)
                for bound in [[],[0],[0,7],list(range(8)),list(range(64)),[0,63],list(range(63))]:
                    for count in [8,64]:add(raw,'reserve',bound=bound,slot_count=count)
                for mapping in [[],[[0,1]],[[0,7]],[[7,0]],[[0,63]],[[0,8]],[[0,1],[7,1]]]:
                    for count in [8,64]:add(raw,'relocate',mapping=mapping,slot_count=count)
    for capture in args.reference.glob('*.gpa_frame'):
        with Frame(capture) as frame:
            for e in frame.entries.values():
                if e.category!=5 or not 0x90<=e.type<=0x95:continue
                resource=frame.resource(e.id)
                if not resource['data_id']:continue
                raw=frame.shader(resource['data_id']);library[hashlib.sha256(raw).hexdigest()]=raw
    for raw in library.values():
        info=shaders.inspect(raw)
        if info['stage']=='ps':
            for slot in [0,1,7]:
                add(raw,'add',slot=slot)
                for alpha in [False,True]:add(raw,'replace',slot=slot,keep_alpha=alpha)
            parts=shaders.chunks(raw);parts.pop('RDEF',None);stripped=dxbc_patch.container(parts)
            add(stripped,'replace',slot=0,array_index=0,array_routed=False)
        add(raw,'relocate',mapping=[],slot_count=64)
        add(raw,'relocate',mapping=[[0,1]],slot_count=64)
        if info['stage']=='ps':add(raw,'reserve',bound=[0,7],slot_count=64)
    seed=shaders.compile_hlsl(sources[0],'ps_5_0')[0]
    for slot in [-1,8,0xffffffff]:
        add(seed,'add',slot=slot);add(seed,'replace',slot=slot)
    for index in [-1,2**32,True,1.5]:add(seed,'replace',slot=0,array_index=index,array_routed=True)
    add(seed,'replace',slot=0,keep_alpha=True,array_index=1)
    for source in [None,dict(register=32,mask=1,component_type=1),dict(register=1,mask=3,component_type=1),dict(register=1,mask=1,component_type=3)]:
        add(seed,'replace',slot=0,array_index=1,array_routed=True,array_source=source)
    for count in [0,9,63]:add(seed,'relocate',mapping=[],slot_count=count)
    # Walk extended, nested and immediate64 operands without treating their data
    # as UAV register tokens. Malformed forms must fail at checked boundaries.
    decl=[0x0300009d,0x0011e000,0]
    data_variants=[[0x4002,0x0011e000,0,0xffffffff,7],[0x5002,0x0011e000,0,0xffffffff,7],
                   [0x5001,0],[0x4000],[0x00104001,0,0],[0x00200006,0,1],
                   [0x00a00006,0,0x0010000a,0],[0x80004001,0,7],[0x80004001]]
    def program(ops):
        parts=shaders.chunks(seed);words=[0x50,0]+[v for row in ops for v in row];words[1]=len(words)
        parts['SHEX']=struct.pack('<'+'I'*len(words),*words);return dxbc_patch.container(parts)
    for data in data_variants:
        op=[166,0x0011e000,0,0x4001,0]+data;op[0]|=len(op)<<24
        add(program([decl,op,[0x0100003e]]),'relocate',mapping=[[0,7]])
    for opcode in [225,226,227]:add(program([[opcode|(1<<24)]]),'relocate',mapping=[])
    for temps in [[],[[0x02000068,4095]],[[0x02000068,4096]],[[0x02000068,1],[0x02000068,2]]]:
        raw=program(temps+[[0x03000065,0x001020f2,0],[0x0304003f,0x0010000a,0],[0x0100003e],[0x0300002c,0x0010a000,0],[0x0100003e]])
        add(raw,'replace',slot=0,array_index=3,array_routed=True,array_source=dict(register=1,mask=2,component_type=1))
    # Reflected ranges must remain contiguous and declared registers cannot alias.
    uav=shaders.compile_hlsl(sources[5],'ps_5_0')[0];parts=shaders.chunks(uav);reflection=bytearray(parts['RDEF'])
    count,offset=struct.unpack_from('<2I',reflection,8);assert count==1
    struct.pack_into('<I',reflection,offset+24,2);parts['RDEF']=bytes(reflection);raw=dxbc_patch.container(parts)
    add(raw,'relocate',mapping=[[0,2]],slot_count=8)
    add(raw,'relocate',mapping=[[0,2],[1,3]],slot_count=8)
    for tag in ['OSG1','OSG5']:
        parts=shaders.chunks(seed);parts[tag]=parts.pop('OSGN');add(dxbc_patch.container(parts),'replace',slot=0)
    env=dict(os.environ);env['PATH']=str(args.qt_bin.resolve())+os.pathsep+str(Path(os.environ['SystemRoot'])/'System32')
    checks=[]
    # Bounded batches keep captured shader payloads out of process command lines.
    for start in range(0,len(jobs),100):
        batch=args.out/f'batch-{start}.json';output=args.out/f'batch-{start}-result.json'
        batch.write_text(json.dumps(jobs[start:start+100]),'utf-8')
        run=subprocess.run([str(args.probe.resolve()),'--probe',str(batch),str(output)],env=env,capture_output=True,timeout=90)
        if run.returncode:raise RuntimeError(run.stderr.decode('utf-8',errors='replace'))
        actual=json.loads(output.read_text());assert len(actual)==len(jobs[start:start+100])
        for offset,got in enumerate(actual):
            index=start+offset;expected=wanted[index];passed=('error' in got) if 'error' in expected else got==expected
            checks.append(dict(index=index,action=jobs[index]['action'],rejected='error' in expected,passed=passed))
            if not passed:(args.out/f'mismatch-{index}.json').write_text(json.dumps(dict(job=jobs[index],expected=expected,actual=got),indent=2),'utf-8')
    report=dict(passed=all(x['passed'] for x in checks),cases=len(checks),shaders=len(library),rejected=sum(x['rejected'] for x in checks),checks=checks)
    (args.out/'validation.json').write_text(json.dumps(report,indent=2),'utf-8')
    print(json.dumps({k:v for k,v in report.items() if k!='checks'}));return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
