"""Pinned original ERG classification, cache membership and CSUAV collectors.

Isolated CPU-only private-ABI experiments. Complete records, bounded native map
allocations, no fake COM objects, playback, injection, or persistent patching.
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
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True,exist_ok=False)
    path=Path('C:/Program Files/IntelSWTools/GPA/dx11_player.dll')
    if hashlib.sha256(path.read_bytes()).hexdigest()!=PLAYER: raise ValueError('Unknown player')
    report=dict(schema='FloraGPA original cache membership probe 1',completed=False,passed=False,
                gpu_execution=False,player_sha256=PLAYER,source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                membership=[],collectors=[])
    def save(): (args.out/'cache.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    save()
    with os.add_dll_directory(str(path.parent)):
        dll=c.WinDLL(str(path));base=dll._handle
        ptr,u32,u64=c.c_void_p,c.c_uint32,c.c_uint64
        fn=lambda rva,result,*params:c.WINFUNCTYPE(result,*params)(base+rva)
        classify=fn(0x453a0,c.c_ubyte,ptr,c.c_uint16)
        actual=[i for i in range(65536) if classify(None,i)]
        expected=list(range(1,255))+list(range(513,767))
        if actual!=expected: raise ValueError('Different original ERG classification')
        report['classification']=dict(domain=65536,accepted_types=actual,passed=True)
        category=fn(0x97dc0,ptr,ptr,ptr)
        descriptor={k:fn(rva,ptr,ptr,ptr) for k,rva in [(1,0x97c90),(2,0x97b60),(3,0x97a30),(4,0x97900)]}
        offsets={1:0x68,2:0x78,3:0x58,4:0x88}
        contains=fn(0x9b8e0,c.c_ubyte,ptr,u32)
        factory=fn(0x3c550,ptr,ptr,ptr,u32,c.c_uint16)
        # Missing category, missing descriptor, wrong descriptor category, invalid
        # category, all four valid kinds, and a registered zero are distinct cases.
        specs=[(0,2,2),(10,1,1),(20,2,2),(30,3,3),(40,4,4),
               (50,3,None),(60,2,1),(70,None,2),(80,9,2)]
        holder=c.create_string_buffer(0x48+0xa0);cache=c.addressof(holder)+0x48
        manager=c.create_string_buffer(0x50);u64.from_address(c.addressof(manager)+0x48).value=c.addressof(holder)
        keep=[holder,manager]
        for offset in [0x48,0x58,0x68,0x78,0x88]:
            sentinel=c.create_string_buffer(0x50);keep.append(sentinel);head=c.addressof(sentinel)
            for field in [0,8,16]: u64.from_address(head+field).value=head
            c.c_uint16.from_address(head+24).value=0x101
            u64.from_address(cache+offset).value=head
        for ident,kind,desc_kind in specs:
            key=u32(ident)
            if kind is not None: u32.from_address(category(cache+0x48,c.byref(key))).value=kind
            if desc_kind is not None: descriptor[desc_kind](cache+offsets[desc_kind],c.byref(key))
        known={ident for ident,kind,desc in specs if kind in offsets and kind==desc}
        for ident in [0,1,10,20,30,40,50,60,70,80,0xffffffff]:
            actual=bool(contains(cache,ident));expected=ident in known
            report['membership'].append(dict(id=ident,actual=actual,expected=expected))
            if actual!=expected: raise ValueError('Membership disagrees')
        report['registry_cases']=[dict(id=i,category=k,descriptor_kind=d) for i,k,d in specs]
        vectors=[[],[0],[10,20,30,40],[50,60,70,80,1,0xffffffff],
                 [20,10,20,0,40,50],[(1<<32)|20,1<<32,0xffffffffffffffff],
                 [40,30,20,10,20]]
        for present in [False,True]:
            for counts in [False,True]:
                for uavs in vectors:
                    raw=struct.pack('<QQIIB',0,0x100000005,0,len(uavs),present)
                    if present: raw+=b''.join(struct.pack('<Q',v) for v in uavs)
                    raw+=struct.pack('<B',counts)
                    if counts: raw+=b''.join(struct.pack('<I',0xffffffff) for _ in uavs)
                    shared=(u64*2)();factory(c.addressof(manager),shared,100,0x25e);obj=shared[0]
                    vt=u64.from_address(obj).value
                    if u64.from_address(vt+32).value!=base+0x23c30: raise ValueError('Wrong CSUAV collector')
                    backing=c.create_string_buffer(raw);start=c.addressof(backing)
                    vec=(u64*3)(start,start+len(raw),start+len(raw));ref=(u64*2)(c.addressof(vec),0)
                    output=(u32*128)();start=c.addressof(output);dest=(u64*3)(start,start,start+c.sizeof(output))
                    fn(0x23c30,None,ptr,ptr,ptr)(obj,ref,dest)
                    if dest[0]!=start or not start<=dest[1]<=dest[2] or dest[2]!=start+c.sizeof(output):
                        raise ValueError('Unexpected output vector')
                    actual=list(output[:(dest[1]-start)//4])
                    expected=[5]+([v&0xffffffff for v in uavs if v&0xffffffff and v&0xffffffff in known] if present else [])
                    report['collectors'].append(dict(present=present,initial_counts_present=counts,uavs=uavs,
                                                     actual=actual,expected=expected,passed=actual==expected))
                    save()
                    if actual!=expected: raise ValueError('CSUAV collector disagrees')
    report.update(completed=True,passed=True)
    save()
    print(json.dumps(dict(passed=True,classification_domain=65536,membership=len(report['membership']),collectors=len(report['collectors']))))


if __name__=='__main__': main()
