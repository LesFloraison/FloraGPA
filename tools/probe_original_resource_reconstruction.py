"""Probe successful original GPA resource reconstruction without a device or capture.

Original resource constructors provide real RTTI. Synthetic resource tables and
the original bound ID callback drive the dispatcher; bounded outputs are copied
before the child exits. Child-local allocations are reclaimed by process teardown.
"""
import argparse
import ctypes as c
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

from probe_original_host_resource_route import Block, tree, SHA, CODE, digest, write


DESCRIPTORS = {
    0x84: (0xb26f0, [4, 1, 1, 28, 0, 8, 0, 0]),
    0x85: (0xb2720, [4, 4, 1, 1, 28, 1, 0, 0, 8, 0, 0]),
    0x86: (0xb2750, [4, 4, 4, 1, 28, 0, 8, 0, 0]),
    0x88: (0xb2640, [0, 1, 1, 1, 0, 1, 8, 0, 0, 0, 0, 0, 0x7f7fffff]),
}


def vector(address, stride, limit=65536):
    begin, end, capacity = struct.unpack('<3Q', c.string_at(address, 24))
    if not (begin <= end <= capacity and capacity - begin <= limit and (end - begin) % stride == 0):
        raise ValueError('Invalid bounded original output vector')
    return begin, c.string_at(begin, end - begin) if begin else b''


def child(args):
    binary = args.gpa / 'dx11_player.dll'
    if digest(binary) != SHA or c.sizeof(c.c_void_p) != 8:
        raise ValueError('Expected pinned x64 player')
    report = dict(schema=1, completed=False, engine_recovered=False, gpu_executed=False,
                  player_sha256=SHA, source_sha256=digest(Path(__file__)), cases=[])
    output = args.out / 'reconstruction.json'
    with os.add_dll_directory(str(args.gpa)):
        library = c.WinDLL(str(binary))
        base = library._handle
        for rva, code in CODE.items():
            if c.string_at(base + int(rva, 16), 32).hex() != code:
                raise ValueError('Original route code changed')
        route = c.WINFUNCTYPE(c.c_ubyte, c.c_void_p, c.c_void_p, c.c_void_p)(base + 0xabac0)
        for kind, (constructor, descriptor) in DESCRIPTORS.items():
            for pattern in (0xa5, 0x5a):
                host = Block(0x3d0, pattern)
                request, resource, slot = Block(0xa8, pattern), Block(0x80, pattern), Block(8, pattern)
                first, second, owner, generator, player = [Block(64, pattern) for _ in range(5)]
                for block, size in ((request, 0xa8), (first, 24), (second, 24)):
                    c.memset(block.address, 0, size)
                constructor_call = c.WINFUNCTYPE(c.c_void_p, c.c_void_p, c.c_uint32)(base + constructor)
                if constructor_call(resource.address, 1) != resource.address:
                    raise ValueError('Original constructor returned another object')
                for n, value in enumerate(descriptor):
                    resource.put(0x1c + n * 4, value, 'I')
                table_rva = c.c_uint64.from_address(resource.address).value - base
                slot.put(0, resource.address)
                resource_calls = []

                @c.WINFUNCTYPE(c.c_void_p, c.c_void_p, c.c_uint32)
                def resolve(this, selector):
                    resource_calls.append(dict(this=this, selector=selector))
                    return slot.address

                resource_table = (c.c_void_p * 4)(None, None, None, c.cast(resolve, c.c_void_p).value)
                # Match the original caller's bound function layout. A nonzero
                # seed exercises the decoded increment path without a singleton.
                player.put(0xc, 999, 'I')
                generator.put(0, base + 0x184c08)
                generator.put(8, base + 0xab6a0)
                generator.put(16, player.address)
                owner.put(0x38, generator.address)
                file_head, file_nodes = tree(pattern, 28, lambda node, key: node.put(40, kind, 'H'))
                api_head, api_nodes = tree(pattern, 32,
                                          lambda node, key: node.put(40, c.addressof(resource_table)))
                host.put(0x360, file_head.address)
                host.put(0xc0, api_head.address)
                request.put(8, 1, 'I')
                request.put(0x18, 28, 'I')
                request.put(0x24, 1, 'I')
                request.put(0x2c, 1, 'I')
                request.put(0x58, host.address)
                request.put(0x68, owner.address)
                if kind == 0x88:
                    request.put(0x40, resource.address + 0x1c)
                blocks = dict(host=host, resource=resource, request=request, first=first, second=second,
                              generator=generator, player=player, owner=owner, slot=slot,
                              file_head=file_head, api_head=api_head,
                              **{f'file_{n}': b for n, b in enumerate(file_nodes)},
                              **{f'api_{n}': b for n, b in enumerate(api_nodes)})
                before = {name: b.image() for name, b in blocks.items()}
                report['active_case'] = dict(kind=kind, pattern=pattern)
                write(output, report)
                result = route(request.address, first.address, second.address)
                _, records = vector(first.address, 15)
                _, pointers = vector(second.address, 16)
                payloads = []
                for i in range(0, len(pointers), 16):
                    pointer = struct.unpack_from('<Q', pointers, i)[0]
                    _, payload = vector(pointer, 1)
                    payloads.append(payload.hex())
                report['cases'].append(dict(kind=kind, pattern=pattern, constructor=constructor,
                    table_rva=table_rva, descriptor=descriptor, result=result,
                    id_callback=dict(table_rva=0x184c08, invoke_rva=0xb1510, target_rva=0xab6a0,
                                     seed=999, final=c.c_uint32.from_address(player.address + 0xc).value),
                    resource_calls=resource_calls, addresses={name: b.address for name, b in blocks.items()},
                    before=before, after={name: b.image() for name, b in blocks.items()},
                    records=records.hex(), payloads=payloads))
                write(output, report)
    report.pop('active_case', None)
    report['completed'] = digest(binary) == SHA
    write(output, report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpa', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--child', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.out, args.gpa = args.out.resolve(), args.gpa.resolve()
    if args.child:
        child(args)
        return
    args.out.mkdir(parents=True, exist_ok=False)
    command = [sys.executable, str(Path(__file__).resolve()), '--out', str(args.out),
               '--gpa', str(args.gpa), '--child']
    run = dict(command=command, timeout_seconds=30, timed_out=False, completed=False)
    start = time.monotonic()
    try:
        with (args.out / 'child.log').open('wb') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=30)
        run.update(exit_code=result.returncode, completed=result.returncode == 0)
    except subprocess.TimeoutExpired:
        run['timed_out'] = True
    run['elapsed_ms'] = round((time.monotonic() - start) * 1000)
    write(args.out / 'run.json', run)
    if not run['completed']:
        raise SystemExit('Original reconstruction probe did not complete')
    print(json.dumps(run))


if __name__ == '__main__':
    main()
