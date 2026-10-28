"""Read the pinned original shader-debug availability property in an isolated child.

No application object is constructed, frame opened or shader executed. This
probes a verified constant property getter, not the missing debugger engine.
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

PINNED = '35a03995d50f390ed74722385583e7bb011f84e563a579c88086d8039ad1df80'
RELATIVE = Path('com/intel/framedb/framedb_qt.dll')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Image:
    def __init__(self, path):
        self.data = path.read_bytes()
        if hashlib.sha256(self.data).hexdigest() != PINNED:
            raise ValueError('Pinned framedb image changed')
        pe, = struct.unpack_from('<I', self.data, 60)
        machine, count = struct.unpack_from('<HH', self.data, pe + 4)
        optional, = struct.unpack_from('<H', self.data, pe + 20)
        self.base, = struct.unpack_from('<Q', self.data, pe + 48)
        if machine != 0x8664 or self.base != 0x180000000:
            raise ValueError('Unexpected original image layout')
        self.sections = [struct.unpack_from('<IIII', self.data, pe + 24 + optional + i * 40 + 8)
                         for i in range(count)]

    def read(self, rva, size):
        for virtual_size, address, raw_size, raw in self.sections:
            if address <= rva and size <= raw_size and rva - address <= raw_size - size:
                return self.data[raw + rva - address:raw + rva - address + size]
        raise ValueError('Read outside original image sections')

    def words(self, rva, count):
        return struct.unpack('<' + 'I' * count, self.read(rva, count * 4))

    def metadata(self):
        pointers = struct.unpack('<8Q', self.read(0x774f00, 64))
        if pointers[2:5] != (self.base + 0x7754c0, self.base + 0x775d50, self.base + 0xb66d0):
            raise ValueError('Unexpected presenter metadata pointers')
        words = self.words(0x775d50, 327)
        if words[:14] != (12, 0, 0, 0, 25, 14, 22, 217, 0, 0, 0, 0, 0, 15):
            raise ValueError('Unexpected presenter metadata header')
        def string(index):
            offset, length = self.words(0x7754c0 + index * 8, 2)
            return self.read(0x7754c0 + offset, length).decode('utf-8')
        properties = []
        for index in range(22):
            name, kind, flags, notify, revision = words[217 + 5 * index:222 + 5 * index]
            properties.append(dict(index=index, name=string(name), type=kind, flags=flags,
                                   notify=notify, revision=revision))
        methods = []
        for index in range(25):
            name, argc, parameters, tag, flags, meta_types = words[14 + 6 * index:20 + 6 * index]
            methods.append(dict(index=index, name=string(name), argc=argc, parameters=parameters,
                                tag=tag, flags=flags, meta_types=meta_types))
        if string(0) != 'ShadersPresenter' or properties[21]['name'] != 'isShaderDebuggerAvailable' or properties[21]['type'] != 1 or methods[23]['name'] != 'debugShader':
            raise ValueError('Unexpected property or method selection')
        if self.read(0xd22e0, 3) != bytes.fromhex('32c0c3'):
            raise ValueError('Availability getter is no longer the verified constant function')
        return dict(class_name=string(0), metaobject_rva='0x774f00', strings_rva='0x7754c0',
                    data_rva='0x775d50', dispatcher_rva='0xb66d0', getter_rva='0xd22e0',
                    getter_bytes=self.read(0xd22e0, 3).hex(), properties=properties, methods=methods)


def child(args):
    path = args.gpa / RELATIVE
    image = Image(path)
    metadata = image.metadata()
    with os.add_dll_directory(str(args.gpa)), os.add_dll_directory(str(path.parent)):
        library = c.WinDLL(str(path))
        base = library._handle
        if c.string_at(base + 0xd22e0, 3) != image.read(0xd22e0, 3):
            raise ValueError('Loaded getter differs from pinned image')
        direct = c.WINFUNCTYPE(c.c_ubyte, c.c_void_p)(base + 0xd22e0)
        dispatcher = c.WINFUNCTYPE(None, c.c_void_p, c.c_int, c.c_int, c.POINTER(c.c_void_p))(base + 0xb66d0)
        rows = []
        for seed in (0, 0x55, 0xff):
            for object_value in (0, 1, 0xffffffffffffffff):
                storage = (c.c_ubyte * 33)(*([seed] * 33))
                before = bytes(storage)
                arguments = (c.c_void_p * 1)(c.addressof(storage) + 16)
                returned = direct(object_value)
                dispatcher(object_value, 1, 21, arguments)
                expected = before[:16] + b'\0' + before[17:]
                if returned != 0 or bytes(storage) != expected or arguments[0] != c.addressof(storage) + 16:
                    raise ValueError('Original constant getter or guarded property result differs')
                rows.append(dict(seed=seed, object_argument=hex(object_value), direct=returned,
                                 before=before.hex(), after=bytes(storage).hex(), property_index=21,
                                 call_kind=1, guard_bytes=16))
        if digest(path) != PINNED:
            raise ValueError('Installed image changed after observation')
        report = dict(schema=1, scope='original_shader_debug_availability_property', binary=path.as_posix(),
                      sha256=PINNED, metadata=metadata, cases=rows, passed=True, engine_recovered=False,
                      live_application=False, frame_opened=False, shader_executed=False)
        (args.out / 'availability.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpa', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--child', action='store_true')
    args = parser.parse_args()
    args.gpa = args.gpa.resolve(); args.out = args.out.resolve()
    if args.child:
        child(args); return
    args.out.mkdir(parents=True, exist_ok=False)
    command = [sys.executable, str(Path(__file__).resolve()), '--gpa', str(args.gpa), '--out', str(args.out), '--child']
    manifest = dict(command=command, source_sha256=digest(Path(__file__)), completed=False)
    start = time.monotonic()
    try:
        with (args.out / 'child.log').open('wb') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=30)
        manifest.update(completed=True, exit_code=result.returncode)
    except subprocess.TimeoutExpired:
        manifest.update(timed_out=True)
    finally:
        manifest['elapsed_ms'] = round((time.monotonic() - start) * 1000)
        (args.out / 'run.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    if manifest.get('exit_code') != 0:
        raise SystemExit('Original availability child failed; inspect retained log')
    print('Validated nine guarded original availability reads')


if __name__ == '__main__':
    main()
