"""Exercise pinned original resource-request routing with guarded synthetic objects.

Research only: no capture, GPU, private debugger engine or installed-file writes.
Unknown resource types avoid RTTI/resource reconstruction; all executed callbacks
belong to this probe. A bounded child isolates original machine-code execution.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

SHA = '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b'
GUARD = 64
CODE = {
    '0xabac0': '48895c240848896c2410488974241857415641574883ec708b6908488bd9488b',
    '0x865a0': '4881ecc8010000488b050a5e17004833c448898424b00100004c8b8188010000',
    '0x85f40': '4881ecc8020000488b056a6417004833c448898424b00200004c8b8188010000',
    '0x9b350': '4881ecc8020000488b055a1016004833c448898424b002000085d2750433c0eb',
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, data):
    path.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')


class Block:
    def __init__(self, size, pattern):
        self.storage = c.create_string_buffer(bytes([pattern]) * (size + 2 * GUARD), size + 2 * GUARD)
        self.address = c.addressof(self.storage) + GUARD

    def put(self, offset, value, kind='Q'):
        raw = struct.pack('<' + kind, value)
        c.memmove(self.address + offset, raw, len(raw))

    def image(self):
        return self.storage.raw.hex()


def tree(pattern, key_offset, value):
    head = Block(64, pattern)
    nodes = [Block(64, pattern) for _ in range(3)]
    keys = [1, 0x80000000, 0xffffffff]
    for node, key in zip(nodes, keys):
        node.put(0, head.address)
        node.put(16, head.address)
        node.put(25, 0, 'B')
        node.put(key_offset, key, 'I')
        value(node, key)
    nodes[1].put(0, nodes[0].address)
    nodes[1].put(16, nodes[2].address)
    head.put(8, nodes[1].address)
    head.put(25, 1, 'B')
    return head, nodes


def child(args):
    binary = args.gpa / 'dx11_player.dll'
    if c.sizeof(c.c_void_p) != 8 or digest(binary) != SHA:
        raise ValueError('Expected pinned x64 player')
    report = dict(schema=1, completed=False, scope='guarded_original_resource_request_routes',
                  engine_recovered=False, gpu_executed=False, player_sha256=SHA,
                  source_sha256=digest(Path(__file__)), cases=[])
    output = args.out / 'routes.json'
    write(output, report)
    with os.add_dll_directory(str(args.gpa)):
        library = c.WinDLL(str(binary))
        base = library._handle
        route = c.WINFUNCTYPE(c.c_ubyte, c.c_void_p, c.c_void_p, c.c_void_p)(base + 0xabac0)
        contains = c.WINFUNCTYPE(c.c_ubyte, c.c_void_p, c.c_uint32)(base + 0x865a0)
        record = c.WINFUNCTYPE(c.c_void_p, c.c_void_p, c.c_uint32)(base + 0x85f40)
        resource = c.WINFUNCTYPE(c.c_void_p, c.c_void_p, c.c_uint32)(base + 0x9b350)
        report['code_prefixes'] = {hex(rva): c.string_at(base + rva, 32).hex()
                                   for rva in (0xabac0, 0x865a0, 0x85f40, 0x9b350)}
        if report['code_prefixes'] != CODE:
            raise ValueError('Loaded original resource-route code changed')
        for pattern in (0xa5, 0x5a):
            for index, identifier in enumerate((1, 0x80000000, 0xffffffff)):
                for mode in ('missing', 'null', 'unknown'):
                    for alias in (False, True):
                        host, request = Block(0x3d0, pattern), Block(0xa8, pattern)
                        first, second = Block(24, pattern), Block(24, pattern)
                        marker, slot = Block(64, pattern), Block(8, pattern)
                        slot.put(0, 0 if mode == 'null' else marker.address)
                        calls = []

                        @c.WINFUNCTYPE(c.c_void_p, c.c_void_p, c.c_uint32)
                        def callback(this, selector):
                            calls.append(dict(this=this, selector=selector))
                            return slot.address

                        table = (c.c_void_p * 4)(None, None, None, c.cast(callback, c.c_void_p).value)
                        file_head, file_nodes = tree(pattern, 28, lambda node, key: node.put(40, 0x7777, 'H'))
                        api_head, api_nodes = tree(pattern, 32,
                                                  lambda node, key: node.put(40, c.addressof(table)))
                        host.put(0x360, file_head.address)
                        host.put(0xc0, api_head.address)
                        request.put(8, identifier if mode != 'missing' else 2, 'I')
                        request.put(0x58, host.address)
                        for offset, values in ((0x78, (11, 12, 13)), (0x90, (21, 22, 23))):
                            for n, value in enumerate(values):
                                request.put(offset + n * 8, value)
                        for n in range(3):
                            first.put(n * 8, 31 + n)
                            second.put(n * 8, 41 + n)
                        blocks = dict(host=host, request=request, first=first, second=second,
                                      marker=marker, slot=slot, file_head=file_head, api_head=api_head,
                                      **{f'file_{n}': b for n, b in enumerate(file_nodes)},
                                      **{f'api_{n}': b for n, b in enumerate(api_nodes)})
                        before = {name: block.image() for name, block in blocks.items()}
                        lookups = dict(found=contains(host.address + 0x1d8, identifier),
                                       absent=contains(host.address + 0x1d8, 2),
                                       file=record(host.address + 0x1d8, identifier),
                                       api=resource(host.address + 0x48, identifier),
                                       null_api=resource(host.address + 0x48, 0))
                        result = route(request.address,
                                       request.address + 0x78 if alias else first.address,
                                       request.address + 0x90 if alias else second.address)
                        row = dict(pattern=pattern, identifier=identifier, mode=mode, alias=alias,
                                   addresses={name: block.address for name, block in blocks.items()},
                                   before=before, after={name: b.image() for name, b in blocks.items()},
                                   lookups=lookups, result=result, callbacks=calls, selected_node=index)
                        report['cases'].append(row)
        report['completed'] = True
        write(output, report)
    if digest(binary) != SHA:
        raise ValueError('Installed player changed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpa', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--child', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.gpa, args.out = args.gpa.resolve(), args.out.resolve()
    if args.child:
        child(args)
        return
    args.out.mkdir(parents=True, exist_ok=False)
    command = [sys.executable, str(Path(__file__).resolve()), '--gpa', str(args.gpa),
               '--out', str(args.out), '--child']
    run = dict(command=command, timeout_seconds=30, timed_out=False, completed=False)
    start = time.monotonic()
    try:
        with (args.out / 'child.log').open('wb') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=30)
        run.update(exit_code=result.returncode, completed=result.returncode == 0)
    except subprocess.TimeoutExpired:
        run['timed_out'] = True
    finally:
        run['elapsed_ms'] = round((time.monotonic() - start) * 1000)
        write(args.out / 'run.json', run)
    if not run['completed']:
        raise SystemExit('Original resource-route probe did not complete')
    print(json.dumps(run))


if __name__ == '__main__':
    main()
