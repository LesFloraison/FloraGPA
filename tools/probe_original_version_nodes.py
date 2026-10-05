"""CPU-only original version allocation, shared-object copy and dependency rebuild.

Only complete bounded records reach original collectors. No initializer, GPU
resource or COM callback is synthesized. Native allocations stay owned by this
isolated process until exit. This proves primitives, not capture execution.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import struct
from probe_original_reference_collectors import PLAYER


def main():
    p=argparse.ArgumentParser(__doc__);p.add_argument('--out',type=Path,required=True)
    args=p.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    path=Path('C:/Program Files/IntelSWTools/GPA/dx11_player.dll')
    if hashlib.sha256(path.read_bytes()).hexdigest()!=PLAYER:raise ValueError('Unknown original player')
    report=dict(schema='FloraGPA native version node primitives 1',completed=False,passed=False,
                player_sha256=PLAYER,source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                gpu_execution=False,original_capture_acceptance=False,cases=[])
    def save():(args.out/'nodes.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    save()
    with os.add_dll_directory(str(path.parent)):
        dll=c.WinDLL(str(path));base=dll._handle;ptr,u64,u32=c.c_void_p,c.c_uint64,c.c_uint32
        fn=lambda rva,result,*params:c.WINFUNCTYPE(result,*params)(base+rva)
        def values(header):
            head=u64.from_address(header).value;expected=u64.from_address(header+8).value
            result=[];seen=set()
            def visit(node):
                if node==head:return
                if not node or node in seen or len(seen)>=64 or c.c_ubyte.from_address(node+25).value:
                    raise ValueError('Invalid node set')
                seen.add(node);visit(u64.from_address(node).value)
                result.append(u32.from_address(node+28).value);visit(u64.from_address(node+16).value)
            visit(u64.from_address(head+8).value)
            if len(result)!=expected:raise ValueError('Native node set count differs')
            return result
        rules=[('state',3,0x97c90,0x9d310,0x3ea00,0x9c300),
               ('resource',0x88,0x97b60,0x9d230,0x3d7a0,0x9c1a0),
               ('erg',0x3e,0x97a30,0x9d150,0x3c550,0x9c250),
               ('data',0x84,0x97900,0x9d070,0x3bfc0,0x9c1a0)]
        for kind,wire,desc_rva,version_rva,factory_rva,collect_rva in rules:
            header=(u64*2)();sentinel=c.create_string_buffer(0x50);head=c.addressof(sentinel)
            for offset in [0,8,16]:u64.from_address(head+offset).value=head
            c.c_uint16.from_address(head+24).value=0x101;header[0]=head
            ident=u32(100);desc=fn(desc_rva,ptr,ptr,ptr)(header,c.byref(ident))
            version=fn(version_rva,ptr,ptr,u64)
            def at(n):return version(desc,n)
            objects=[];case=dict(kind=kind,type=wire,steps=[]);report['cases'].append(case)
            def assign(n):
                shared=(u64*2)();fn(factory_rva,ptr,ptr,ptr,u32,c.c_uint16)(None,shared,100,wire)
                if not shared[0]:raise ValueError('Empty original object')
                fn(0x9d020,None,ptr,ptr)(at(n),shared);objects.append(shared[0]);return shared[0]
            def snapshot(n):
                node=at(n);obj=u64.from_address(node+48).value
                return dict(status=u32.from_address(node+40).value,dependencies=values(node+8),
                            dependents=values(node+24),object_token=objects.index(obj)+1 if obj else 0)
            def record(label,versions,expected):
                actual={str(n):snapshot(n) for n in versions}
                case['steps'].append(dict(label=label,nodes=actual,expected=expected));save()
                if actual!=expected:raise ValueError('Version primitive mismatch: '+kind+'/'+label)
            def state(deps=(),children=(),token=0,status=0):
                return dict(status=status,dependencies=list(deps),dependents=list(children),object_token=token)
            def collect(n,refs):
                if kind=='state':
                    raw=bytearray(22320);struct.pack_into('<Q',raw,144,refs[0] if refs else 0)
                elif kind=='resource':
                    raw=bytearray(68);struct.pack_into('<Q',raw,8,refs[0] if refs else 0)
                elif kind=='erg':raw=struct.pack('<QQQQ',0,*(refs if refs else [0,0,0]))
                else:raw=struct.pack('<I',len(refs))+b''.join(struct.pack('<Q',i)+bytes(24) for i in refs)+struct.pack('<I',0)
                backing=c.create_string_buffer(bytes(raw));start=c.addressof(backing)
                vec=(u64*3)(start,start+len(raw),start+len(raw));reference=(u64*2)(c.addressof(vec),0)
                fn(collect_rva,None,ptr,ptr)(at(n),reference)
            a=[11,22,11] if kind in ['erg','data'] else [11]
            b=[33,33,33] if kind=='erg' else [33]
            aset=sorted(set(a));union=sorted(set(a+b));empty=[0] if kind=='erg' else []
            record('fresh',[0],{'0':state()})
            assign(0);collect(0,a);fn(0x980c0,None,ptr,u32)(at(0),200)
            original=state(aset,[200],1,1);record('collected',[0],{'0':original})
            at(7) # Actual native growth; versions 1..7 are initially blank.
            record('sparse_growth',[0,1,7],{'0':original,'1':state(),'7':state()})
            fn(0x977d0,ptr,ptr,ptr)(at(7),at(0))
            record('copied',[0,7],{'0':original,'7':original})
            assign(7);collect(7,b);fn(0x980c0,None,ptr,u32)(at(7),201)
            modified=state(union,[200,201],2,1)
            record('recollected',[0,7],{'0':original,'7':modified})
            collect(7,[])
            modified=state(sorted(set(union+empty)),[200,201],2,1)
            record('empty_recollection',[0,7],{'0':original,'7':modified})
            assign(1);collect(1,[])
            record('fresh_empty',[0,1,7],{'0':original,'1':state(empty,[],3,1),'7':modified})
            fn(0x977d0,ptr,ptr,ptr)(at(7),at(0))
            record('copy_replaces_sets',[0,1,7],{'0':original,'1':state(empty,[],3,1),'7':original})
            case['passed']=True
    report.update(completed=True,passed=True);save();print('Native version primitive cases:',len(report['cases']))


if __name__=='__main__':main()
