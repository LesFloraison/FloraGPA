"""Validate nonempty GPA debugger wire records in a bounded research child.

Field names describe offsets, not inferred shader semantics. No engine executes.
All containers are caller-owned, already sized, and use inline strings. Original
code must not allocate, resize, or destroy these synthetic container buffers.
"""
import argparse
import ctypes as c
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

from probe_original_debugger_wire import PLAYER_SHA256, SHA256, digest, require


def fixture(kind, seed):
    serial = 0

    def record(depth=0):
        nonlocal serial
        serial += 1
        identity = serial
        words = [0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc12345,
                 0x7f812345, 0xffffffff, 1]
        raw = struct.pack('<32I', *(words[(i + identity) % 8] ^ seed for i in range(32)))
        return dict(u32_00=(identity ^ seed), u32_04=((identity * 19) ^ seed),
                    string_08=('r%02d' % identity), u32_28=((identity * 37) ^ seed),
                    bytes_30=raw.hex(), byte_b0=(identity * 61 + seed) & 255,
                    records_b8=[record(depth - 1), record()] if depth else [])

    result = dict(records_20=[], records_38=[], records_50=[], string_68='', strings_88=[], string_a0='')
    if kind == 'leaf':
        result['records_20'] = [record(), record()]
    elif kind == 'nested':
        result['records_38'] = [record(2)]
    elif kind == 'groups':
        result['records_50'] = [dict(records_00=[record()], records_18=[record(1)],
                                     records_30=[record()], u32_48=seed, u32_4c=seed ^ 0x12345678),
                                 dict(records_00=[], records_18=[record()], records_30=[],
                                      u32_48=seed ^ 1, u32_4c=seed ^ 0xfedcba98)]
    elif kind == 'mixed':
        result['records_20'] = [record(1)]
        result['records_38'] = [record()]
        result['records_50'] = [dict(records_00=[record()], records_18=[], records_30=[record(1)],
                                     u32_48=seed ^ 3, u32_4c=seed ^ 0x80000000)]
        result.update(string_68='0123456789abcde', strings_88=['first', '', 'A\x00B', 'last'],
                      string_a0='source-sentinel')
    else:
        raise ValueError('Unknown fixture')
    return result


def expected_chunks(model):
    chunks = []

    def integer(value, size):
        chunks.append(value.to_bytes(size, 'little'))

    def string(value):
        data = value.encode('ascii')
        integer(len(data), 8)
        chunks.append(data)

    def records(rows):
        integer(len(rows), 8)
        for row in rows:
            integer(row['u32_00'], 4)
            integer(row['u32_04'], 4)
            string(row['string_08'])
            integer(row['u32_28'], 4)
            chunks.append(bytes.fromhex(row['bytes_30']))
            integer(row['byte_b0'], 1)
            records(row['records_b8'])

    records(model['records_20'])
    records(model['records_38'])
    integer(len(model['records_50']), 8)
    for row in model['records_50']:
        for key in ('records_00', 'records_18', 'records_30'):
            records(row[key])
        integer(row['u32_48'], 4)
        integer(row['u32_4c'], 4)
    string(model['string_68'])
    integer(len(model['strings_88']), 8)
    for value in model['strings_88']:
        string(value)
    string(model['string_a0'])
    return chunks


class Arena:
    """Keep ownership and guards independent from original C++ allocators."""
    def __init__(self, model, populated):
        self.blocks = []
        self.descriptors = []
        self.root = self.allocate('result', 0xc0)
        self.build(self.root, model, 'result', populated)

    def allocate(self, label, size):
        require(size <= 0x10000, 'Arena bound exceeded')
        storage = (c.c_ubyte * (size + 64))(*([0xa5] * (size + 64)))
        address = c.addressof(storage) + 32
        self.blocks.append((label, storage, address, size))
        return address

    def put(self, address, data):
        c.memmove(address, data, len(data))

    def string(self, address, value, populated):
        data = value.encode('ascii') if populated else b''
        require(len(data) <= 15, 'Only inline strings are permitted')
        self.put(address, data + bytes(16 - len(data)) + struct.pack('<QQ', len(data), 15))

    def vector(self, address, rows, stride, label, populated, element):
        start = self.allocate(label, len(rows) * stride) if rows else 0
        descriptor = struct.pack('<QQQ', start, start + len(rows) * stride, start + len(rows) * stride)
        self.put(address, descriptor)
        self.descriptors.append((address, descriptor))
        for index, row in enumerate(rows):
            element(start + index * stride, row, label + '/' + str(index), populated)

    def records(self, address, rows, label, populated):
        self.vector(address, rows, 0xd0, label, populated, self.record)

    def record(self, address, row, label, populated):
        if populated:
            self.put(address, struct.pack('<II', row['u32_00'], row['u32_04']))
            self.put(address + 0x28, struct.pack('<I', row['u32_28']))
            self.put(address + 0x30, bytes.fromhex(row['bytes_30']))
            self.put(address + 0xb0, bytes([row['byte_b0']]))
        self.string(address + 8, row['string_08'], populated)
        self.records(address + 0xb8, row['records_b8'], label + '/records_b8', populated)

    def group(self, address, row, label, populated):
        for offset, key in ((0, 'records_00'), (0x18, 'records_18'), (0x30, 'records_30')):
            self.records(address + offset, row[key], label + '/' + key, populated)
        if populated:
            self.put(address + 0x48, struct.pack('<II', row['u32_48'], row['u32_4c']))

    def build(self, address, model, label, populated):
        self.records(address + 0x20, model['records_20'], label + '/records_20', populated)
        self.records(address + 0x38, model['records_38'], label + '/records_38', populated)
        self.vector(address + 0x50, model['records_50'], 0x50, label + '/records_50', populated, self.group)
        self.string(address + 0x68, model['string_68'], populated)
        self.vector(address + 0x88, model['strings_88'], 0x20, label + '/strings_88', populated,
                    lambda p, r, label, filled: self.string(p, r, filled))
        self.string(address + 0xa0, model['string_a0'], populated)

    def snapshot(self):
        return [dict(label=label, address=address, bytes=size, guarded_hex=bytes(storage).hex())
                for label, storage, address, size in self.blocks]

    def check(self):
        for label, storage, _, _ in self.blocks:
            require(bytes(storage[:32]) == bytes([0xa5] * 32) and
                    bytes(storage[-32:]) == bytes([0xa5] * 32), 'Guard changed: ' + label)
        for address, expected in self.descriptors:
            require(c.string_at(address, 24) == expected, 'Original code resized a synthetic vector')


def child(args):
    binaries = [(args.gpa / 'dx11_player.dll', PLAYER_SHA256), (args.gpa / 'dx11_playback.dll', SHA256)]
    require(c.sizeof(c.c_void_p) == 8 and all(digest(p) == sha for p, sha in binaries), 'Wrong pinned binaries')
    report = dict(schema=1, scope='original_debugger_nonempty_records', completed=False,
                  engine_recovered=False, shader_debug_parity=False, source_sha256=digest(Path(__file__)),
                  binaries=[dict(path=str(p), sha256=sha) for p, sha in binaries], cases=[])
    try:
        with os.add_dll_directory(str(args.gpa.resolve())):
            player, playback = [c.WinDLL(str(p.resolve())) for p, _ in binaries]
        received, sent, reads, writes, errors = bytearray(), bytearray(), [], [], []
        transfer = c.CFUNCTYPE(c.c_uint64, c.c_void_p, c.c_void_p, c.c_uint64)

        @transfer
        def receive(this, destination, count):
            if count > len(received) or count > 65536:
                errors.append('Input overrun')
                return 0
            reads.append(count)
            c.memmove(destination, bytes(received[:count]), count)
            del received[:count]
            return count

        @transfer
        def send(this, source, count):
            if count > 65536 or len(sent) + count > 65536:
                errors.append('Output overrun')
                return 0
            writes.append(count)
            sent.extend(c.string_at(source, count))
            return count

        table = (c.c_void_p * 2)(c.cast(receive, c.c_void_p), c.cast(send, c.c_void_p))
        transport = (c.c_void_p * 1)(c.addressof(table))
        codec = (c.c_uint64 * 3)(0, c.addressof(transport), 0)
        function = c.CFUNCTYPE(None, c.c_void_p, c.c_void_p)
        encode = function(player._handle + 0x10dd30)
        decode = function(playback._handle + 0x262690)
        for kind in ('leaf', 'nested', 'groups', 'mixed'):
            for seed in (0, 0x80000000, 0xffffffff):
                model = fixture(kind, seed)
                chunks = expected_chunks(model)
                expected = b''.join(chunks)
                source, target = Arena(model, True), Arena(model, False)
                source_before, target_before = source.snapshot(), target.snapshot()
                sent.clear()
                writes.clear()
                encode(c.addressof(codec), source.root)
                report['active_case'] = dict(kind=kind, seed=seed, actual_wire_hex=sent.hex(),
                    expected_wire_hex=expected.hex(), write_sizes=writes.copy(),
                    expected_sizes=list(map(len, chunks)), errors=errors.copy())
                require(bytes(sent) == expected and writes == list(map(len, chunks)) and not errors,
                        'Original writer differs from independently constructed packet')
                source.check()
                require(source.snapshot() == source_before, 'Writer changed source memory')
                received.extend(expected)
                reads.clear()
                decode(c.addressof(codec), target.root)
                require(not received and not errors and reads == [len(chunk) for chunk in chunks if chunk],
                        'Reader transfer mismatch')
                decoded_read_sizes = reads.copy()
                target.check()
                decoded = target.snapshot()
                sent.clear()
                writes.clear()
                encode(c.addressof(codec), target.root)
                require(bytes(sent) == expected and not errors and target.snapshot() == decoded,
                        'Cross-module roundtrip mismatch')
                # Existing reader paths skip zero counts and zero-length strings.
                # Observe reuse explicitly instead of claiming an empty wire clears state.
                received.extend(bytes(48))
                reads.clear()
                decode(c.addressof(codec), target.root)
                target.check()
                require(not received and not errors and reads == [8] * 6, 'Empty reuse transfer mismatch')
                require(target.snapshot() == decoded, 'Unexpected empty reuse behavior')
                report['cases'].append(dict(kind=kind, seed=seed, model=model, wire_hex=sent.hex(),
                    write_sizes=writes.copy(), read_sizes=decoded_read_sizes, source_before=source_before,
                    source_after=source.snapshot(), target_before=target_before, target_after=decoded,
                    target_after_empty_reuse=target.snapshot(), empty_reuse_read_sizes=reads.copy()))
        require(all(digest(p) == sha for p, sha in binaries), 'Original files changed')
        report['completed'] = True
        report.pop('active_case', None)
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        (args.out / 'records.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpa', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--child', action='store_true')
    args = parser.parse_args()
    if args.child:
        child(args)
        return
    args.out.mkdir(parents=True, exist_ok=False)
    command = [sys.executable, str(Path(__file__).resolve()), '--gpa', str(args.gpa.resolve()),
               '--out', str(args.out.resolve()), '--child']
    with (args.out / 'child.log').open('w', encoding='utf-8') as log:
        try:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=30,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = None
    (args.out / 'process.json').write_text(json.dumps(dict(command=command, exit_code=code,
        timed_out=code is None, timeout_seconds=30), indent=2) + '\n', encoding='utf-8')
    require(code == 0, 'Original record probe failed; inspect retained reports')
    print('Twelve nonempty record and twelve empty-reuse cases passed; no engine execution claimed')


if __name__ == '__main__':
    main()
