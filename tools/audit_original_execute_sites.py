"""Read-only PE audit of literal ExecuteCommandList slot references.

This is a bounded static search, not a complete proof of program behavior. No
DLL is loaded or modified. Non-literal/computed dispatch is outside this scan.
"""
import argparse
import bisect
import hashlib
import json
from pathlib import Path
import capstone
import pefile

PINNED = {
    'dx11_player.dll': '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b',
    'shimd3d64.dll': 'cb0b99d113511cbf7ca9f949d97aa02e4a1f8f110b1f3ad3165d8a81035dee31',
    'dx11_playback.dll': '0a531905386af6c5f2c0357d4179cec208bb171a287b262f3c4a5539d483bf5e',
}


def scan_code(data, address):
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True
    decoder.skipdata = True
    candidates, skipped = [], []
    total = 0
    instructions = 0
    for ins in decoder.disasm(data, address):
        total += ins.size
        if not ins.id:
            skipped.append(dict(address=ins.address, bytes=ins.bytes.hex()))
            continue
        instructions += 1
        for op in ins.operands:
            if op.type != capstone.x86.X86_OP_MEM or op.mem.disp != 0x1d0:
                continue
            if op.mem.base in [0, capstone.x86.X86_REG_RIP]:
                continue
            direct = ins.mnemonic in ['call', 'jmp']
            if direct or (op.access & capstone.CS_AC_READ and ins.mnemonic != 'lea'):
                candidates.append(dict(address=ins.address, mnemonic=ins.mnemonic, operands=ins.op_str,
                    bytes=ins.bytes.hex(), direct_indirect_transfer=direct,
                    stack_base=op.mem.base in [capstone.x86.X86_REG_RSP,capstone.x86.X86_REG_RBP]))
            break
    if total != len(data):
        raise ValueError('Decoder did not account for the complete input range')
    return dict(instructions=instructions, accounted_bytes=total, skipped_data=skipped, candidates=candidates)


def inspect(path):
    sha = hashlib.sha256(path.read_bytes()).hexdigest()
    if sha != PINNED[path.name]:
        raise ValueError('Unknown binary: '+path.name)
    pe = pefile.PE(str(path))
    if pe.FILE_HEADER.Machine != 0x8664:
        raise ValueError('Expected AMD64 PE')
    base = pe.OPTIONAL_HEADER.ImageBase
    ranges = sorted((e.struct.BeginAddress,e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
    starts = [a for a,b in ranges]
    report = dict(sha256=sha, sections=[], candidates=[])
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:
            continue
        data = section.get_data()
        result = scan_code(data,base+section.VirtualAddress)
        for row in result.pop('candidates'):
            rva = row.pop('address')-base
            index = bisect.bisect_right(starts,rva)-1
            row.update(rva=hex(rva), function_rva=hex(starts[index]) if
                       index >= 0 and rva < ranges[index][1] else None)
            report['candidates'].append(row)
        for row in result['skipped_data']:
            row['rva'] = hex(row.pop('address')-base)
        result.update(name=section.Name.rstrip(b'\0').decode('ascii'), rva=hex(section.VirtualAddress))
        report['sections'].append(result)
    report['direct_transfer_sites'] = [r['rva'] for r in report['candidates'] if r['direct_indirect_transfer']]
    if path.name == 'dx11_player.dll':
        q = lambda rva: int.from_bytes(pe.get_data(rva,8),'little')-base
        slots = {'api_execute':q(0x176f18+8),'erg_execute':q(0x176f50+14*8)}
        empty = pe.get_data(0x166a0,3).hex()
        if set(slots.values()) != {0x166a0} or empty != 'c20000':
            raise ValueError('Different legacy Execute vtable or return bytes')
        report['legacy_execute_slots'] = {key:hex(value) for key,value in slots.items()}
        report['legacy_execute_bytes'] = empty
        if report['direct_transfer_sites'] != ['0x4da2f']:
            raise ValueError('Literal player transfer candidates changed; inspect before classifying')
    if path.name == 'shimd3d64.dll':
        q = lambda rva: int.from_bytes(pe.get_data(rva,8),'little')-base
        table = 0x4cf238
        col = q(table-8)
        descriptor = int.from_bytes(pe.get_data(col+12,4),'little')
        name = pe.get_string_at_rva(descriptor+16).decode('ascii')
        code = pe.get_data(0x2e41a0,14).hex()
        if name != '.?AVDX11DeviceContextWrapper@@' or q(table+58*8) != 0x2e41a0 or code != '488b4908488b0148ffa0d0010000':
            raise ValueError('Different context forwarding thunk')
        report['context_forwarder'] = dict(rtti=name,vtable_rva=hex(table),slot=58,
                                          function_rva='0x2e41a0',bytes=code)
    return report


def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--gpa',type=Path,default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    report=dict(schema='FloraGPA original Execute site audit 1',completed=False,
                source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                dll_loaded=False, gpu_execution=False, traditional_list_execution_accepted=False,
                limits=['Literal displacement scan only; computed dispatch may be absent from candidates.',
                        'Linear sweep includes non-code bytes; skipped bytes and raw candidates are retained.',
                        'A displacement match alone does not establish a DX11 interface call.'], binaries={})
    for name in PINNED:
        report['binaries'][name]=inspect(args.gpa/name)
        (args.out/'sites.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
        print(name,'literal indirect transfer candidates:',len(report['binaries'][name]['direct_transfer_sites']),flush=True)
    report['completed']=True
    (args.out/'sites.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
