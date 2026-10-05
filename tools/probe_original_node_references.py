"""Bounded CPU-only resource/data collector probes; no fake COM or GPU calls.

Device, SwapChain and cached InputLayout paths are deliberately excluded. Buffer
and texture data IDs are zero to avoid native cache dereferences. Full original
playback observations supply those omitted production paths. Native allocations
are retained until this isolated process exits.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import struct
from probe_original_reference_collectors import PLAYER

RESOURCE_RULES = {
    0x83:(48,0x5b960),0x84:(56,0x5c180),0x85:(68,0x5c210),0x86:(60,0x5c300),
    0x87:(68,0x5bc80),0x88:(68,0x5c0c0),0x8b:(68,0x5bbc0),0x8c:(48,0x5c110),
    0x8d:(44,0x5c050),0x8e:(48,0x5bc10),0x8f:(44,0x5c390),0x90:(56,0x5bb20),
    0x92:(56,0x5bb20),0x93:(56,0x5bb20),0x94:(56,0x5bb20),0x95:(56,0x5bb20),
    0x10d:(344,0x5b8c0),0x10f:(64,0x5bfb0),0x127:(24,0x5c410)}


def records():
    for wire,(size,rva) in RESOURCE_RULES.items():
        for mode,value in [('zero',0),('high_zero',1<<32),('duplicates',7),('markers',0x1234567800000009)]:
            raw=bytearray(size)
            for offset in [0,8]: struct.pack_into('<Q',raw,offset,value)
            if wire in [0x8c,0x8d,0x8e,0x8f]:struct.pack_into('<Q',raw,16,value)
            if wire in [0x90,0x92,0x93,0x94,0x95]:
                for offset in [32,40,48]:struct.pack_into('<Q',raw,offset,value+offset if mode=='markers' else value)
            yield 5,wire,rva,mode,raw
        if wire==0x85:
            for usage in [0,1]:
                for original in [0,0x1234567800000009]:
                    raw=bytearray(size);struct.pack_into('<Q',raw,0,original)
                    struct.pack_into('<I',raw,44,usage);struct.pack_into('<I',raw,52,0x80000000)
                    yield 5,wire,rva,f'alias_{usage}_{original}',raw
    def p(fmt,*values):return struct.pack('<'+fmt,*values)
    for value in [0,1<<32,7,0x1234567800000009]:
        cases={1:(0x16870,p('I',3)+b'abc'),0x102:(0x16870,p('I',0)),
            0x100:(0x16970,p('II',2,3)+b'abxyz'),0x101:(0x166c0,p('I',2)+b'ab'+p('I',3)+b'xyz'),
            0x81:(0xb65f0,p('II',2,3)+b'abxyz'+p('I',1)+b'x'+p('IQQ',2,value,value)),
            0x103:(0x16b20,p('QQQI',value,0,0,2)+b'ab'),0x104:(0x45590,p('QI',value,123)),
            0x86:(0xb6a80,p('I',2)+bytes(32)),0x87:(0xba8e0,p('I',2)+bytes(48)),
            0x84:(0x5bdd0,p('I',3)+b''.join(p('Q',i)+bytes(24) for i in [value,0,value])+p('I',2)+b'ab')}
        for wire,(rva,raw) in cases.items():yield 9,wire,rva,f'ids_{value}',raw



def list_records():
    # Full-sized inert records only. The Execute operand is never dereferenced
    # by collector 0x23be0, and the resource pointer is inert for 0x5bad0.
    values = [0, 1 << 32, 7, 0xfedcba9800000009]
    for parent in values:
        for pointer in values:
            mode = f'parent_{parent}_pointer_{pointer}'
            yield 5, 0x9a, 0x5bad0, mode, struct.pack('<QQ', pointer, parent)
            for restore in [0, 1, 2, 0xffffffff]:
                yield 7, 0x41, 0x23be0, mode + f'_restore_{restore}', struct.pack(
                    '<QQQI', 0xabcdef1200000063, parent, pointer, restore)


def main():
    parser=argparse.ArgumentParser(__doc__);parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--lists-only', action='store_true', help='Probe only traditional list dependency collectors; no execution')
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    path=Path('C:/Program Files/IntelSWTools/GPA/dx11_player.dll')
    if hashlib.sha256(path.read_bytes()).hexdigest()!=PLAYER:raise ValueError('Unknown player')
    report=dict(schema='FloraGPA original node reference probes 1',completed=False,passed=False,
                gpu_execution=False,player_sha256=PLAYER,source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                cases=[],excluded_resources=[0x38,0x81,0x82],buffer_texture_data_ids=0,
                scope='traditional_list_metadata' if args.lists_only else 'resource_data_metadata')
    def save():(args.out/'nodes.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    save()
    with os.add_dll_directory(str(path.parent)):
        dll=c.WinDLL(str(path));base=dll._handle;ptr,u64,u32=c.c_void_p,c.c_uint64,c.c_uint32
        # Data 0x104's factory stores manager->holder + 0x48. Its collector
        # 0x45590 only deserializes/appends the owner, never queries that cache.
        # This bounded pointer holder has no virtual functions or COM object.
        holder=c.create_string_buffer(0x48+0xa0);manager=c.create_string_buffer(0x50)
        u64.from_address(c.addressof(manager)+0x48).value=c.addressof(holder)
        for category,wire,rva,mode,raw in (list_records() if args.lists_only else records()):
            factory=c.WINFUNCTYPE(ptr,ptr,ptr,u32,c.c_uint16)(base+{5:0x3d7a0,7:0x3c550,9:0x3bfc0}[category])
            shared=(u64*2)();factory(c.addressof(manager) if category==9 and wire==0x104 else None,shared,100,wire);obj=shared[0]
            if not obj or u64.from_address(u64.from_address(obj).value+(32 if category==7 else 16)).value!=base+rva:
                raise ValueError('Unexpected node collector')
            backing=c.create_string_buffer(bytes(raw));start=c.addressof(backing)
            source=(u64*3)(start,start+len(raw),start+len(raw));ref=(u64*2)(c.addressof(source),0)
            values=(u32*128)();start=c.addressof(values);dest=(u64*3)(start,start,start+c.sizeof(values))
            c.WINFUNCTYPE(None,ptr,ptr,ptr)(base+rva)(obj,ref,dest)
            if dest[0]!=start or dest[2]!=start+c.sizeof(values) or not start<=dest[1]<=dest[2]:
                raise ValueError('Different native output allocation')
            report['cases'].append(dict(category=category,type=wire,collector_rva=hex(rva),mode=mode,
                                       raw_hex=bytes(raw).hex(),references=list(values[:(dest[1]-start)//4])))
            save()
    report.update(completed=True,passed=True);save();print('Native node probe calls:',len(report['cases']))


if __name__=='__main__':main()
