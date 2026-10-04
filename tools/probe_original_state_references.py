"""CPU-only original StateBlock collector on complete marker records.

IDs are inert integers: no GPU or fake COM call, no malformed input, and no DLL
patch. Original allocations remain owned by the isolated process until exit.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import struct
from probe_original_reference_collectors import PLAYER


def state_layout():
    fields={};counts={};offset=128
    def skip(size):
        nonlocal offset
        offset+=size
    def ids(name,count=1):
        nonlocal offset
        for i in range(count):
            fields[name if count==1 else f'{name}[{i}]']=offset;offset+=8
    def number(name): counts[name]=offset;skip(4)
    def stage(name):
        ids(name+'.cb',14);ids(name+'.samplers',16);ids(name+'.shader')
        ids(name+'.srv',128);ids(name+'.classes',256);number(name+'.class_count')
    ids('ib');skip(8);ids('layout');skip(4);ids('vb',32);skip(256)
    for name in ['vs','hs','ds','gs']: stage(name)
    ids('so',4);skip(32);number('so_count');ids('scissors');ids('rasterizer');ids('viewports')
    stage('ps');ids('blend');skip(20);ids('depth_state');skip(4);ids('rtv',8);number('om_start')
    ids('dsv');skip(32);number('rt_count');stage('cs');number('cs_start');number('cs_count')
    ids('cs_uav',8);skip(32);ids('predicate');skip(8);ids('om_extended',56);skip(224)
    ids('cs_extended',56);skip(224)
    assert offset==22320
    return fields,counts


def main():
    parser=argparse.ArgumentParser(__doc__);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    path=Path('C:/Program Files/IntelSWTools/GPA/dx11_player.dll')
    if hashlib.sha256(path.read_bytes()).hexdigest()!=PLAYER: raise ValueError('Unknown player')
    fields,counts=state_layout();names={offset+1:name for name,offset in fields.items()}
    report=dict(schema='FloraGPA original state collector markers 1',completed=False,passed=False,
                player_sha256=PLAYER,gpu_execution=False,source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                fields=fields,counts=counts,cases=[])
    def save(): (args.out/'state.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    save()
    with os.add_dll_directory(str(path.parent)):
        dll=c.WinDLL(str(path));base=dll._handle
        ptr,u64,u32=c.c_void_p,c.c_uint64,c.c_uint32
        factory=c.WINFUNCTYPE(ptr,ptr,ptr,u32)(base+0x3ea00)
        for mode in ['markers','zero_mask','high_word','low_zero','duplicates','zero_class_count']:
            raw=bytearray(22320)
            if mode!='zero_mask':raw[:128]=b'\xff'*128
            for name,offset in fields.items():
                value=(1<<32) if mode=='low_zero' else 7 if mode=='duplicates' else offset+1
                if mode=='high_word':value|=0x8765432100000000
                struct.pack_into('<Q',raw,offset,value)
            for name,offset in counts.items():
                value=256 if name.endswith('.class_count') and mode!='zero_class_count' else 0
                struct.pack_into('<I',raw,offset,value)
            shared=(u64*2)();factory(None,shared,100);obj=shared[0]
            table=u64.from_address(obj).value
            if u64.from_address(table+48).value!=base+0x63d80:raise ValueError('Different state collector')
            backing=c.create_string_buffer(bytes(raw));start=c.addressof(backing)
            source=(u64*3)(start,start+len(raw),start+len(raw));ref=(u64*2)(c.addressof(source),0)
            values=(u32*4096)();start=c.addressof(values);dest=(u64*3)(start,start,start+c.sizeof(values))
            c.WINFUNCTYPE(None,ptr,ptr,ptr)(base+0x63d80)(obj,ref,dest)
            if dest[0]!=start or dest[2]!=start+c.sizeof(values) or not start<=dest[1]<=dest[2]:
                raise ValueError('Different output allocation')
            actual=list(values[:(dest[1]-start)//4])
            labels=[names[v] for v in actual] if mode not in ['duplicates','low_zero'] else []
            report['cases'].append(dict(mode=mode,raw_hex=raw.hex(),references=actual,fields=labels));save()
        marker=report['cases'][0]['references']
        for case in report['cases'][1:]:
            expected=[] if case['mode']=='low_zero' else [7]*len(marker) if case['mode']=='duplicates' else (
                [v for v in marker if '.classes[' not in names[v]] if case['mode']=='zero_class_count' else marker)
            if case['references']!=expected:raise ValueError('Unexpected marker boundary: '+case['mode'])
    report.update(completed=True,passed=True,omitted_fields=sorted(set(fields)-set(report['cases'][0]['fields'])))
    save();print(json.dumps(dict(passed=True,fields=len(fields),registered=len(marker),omitted=len(report['omitted_fields']))))


if __name__=='__main__':main()
