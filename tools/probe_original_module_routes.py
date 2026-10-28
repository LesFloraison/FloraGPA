"""Probe hash-pinned GPA module bindings in a bounded research-only child.

No shader is executed and no debugger implementation is supplied. Installed
files remain read-only; the product and release tests do not use this probe.
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

PINNED = {
    'dx11_player.dll': '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b',
    'ShaderParse.dll': '7a5d68d32f72f3abfb6346126058ff0eec6e9dba0e31d244142c01f026596c82',
}
P, Q = c.c_void_p, c.c_uint64


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(root):
    for name, sha in PINNED.items():
        if digest(root / name) != sha:
            raise ValueError('Pinned research module changed: ' + name)


class Narrow:
    """Read-only borrowed MSVC x64 string, with explicit inline/heap encoding."""
    def __init__(self, text):
        raw = text.encode('ascii')
        self.storage = c.create_string_buffer(32)
        self.heap = None
        if len(raw) <= 15:
            c.memmove(self.storage, raw, len(raw))
            capacity = 15
        else:
            self.heap = c.create_string_buffer(raw)
            Q.from_buffer(self.storage).value = c.addressof(self.heap)
            capacity = len(raw)
        Q.from_buffer(self.storage, 16).value = len(raw)
        Q.from_buffer(self.storage, 24).value = capacity


def child(args):
    verify(args.gpa)
    if c.sizeof(P) != 8:
        raise ValueError('Expected a 64-bit research process')
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.GetModuleHandleW.argtypes, kernel.GetModuleHandleW.restype = [c.c_wchar_p], P
    kernel.GetProcAddress.argtypes, kernel.GetProcAddress.restype = [P, c.c_char_p], P
    kernel.GetModuleFileNameW.argtypes = [P, c.c_wchar_p, c.c_uint32]
    kernel.GetModuleFileNameW.restype = c.c_uint32
    if kernel.GetModuleHandleW('shimloader64.dll'):
        raise ValueError('The direct resolver probe requires no active shim')
    rows, resolutions, directories = [], [], []
    with os.add_dll_directory(str(args.gpa)):
        dll = c.WinDLL(str(args.gpa / 'dx11_player.dll'))
        # The original loader temporarily selects its executable's directory.
        # Pin the dependency explicitly in this Python-hosted research process.
        parser_dll = c.WinDLL(str(args.gpa / 'ShaderParse.dll'))
        base = dll._handle
        constructor = c.WINFUNCTYPE(P, P, Q)(base + 0xfac50)
        unload = c.WINFUNCTYPE(c.c_bool, P)(base + 0xc7f40)
        cleanup = c.WINFUNCTYPE(None, P)(base + 0x384d0)
        resolve = c.WINFUNCTYPE(P, P, P)(base + 0xc6620)
        for pattern in (0xa5, 0x5a):
            for marker in (0, 0x123456789abcdef0, 0xfedcba9876543210):
                storage = c.create_string_buffer(bytes([pattern]) * 136, 136)
                pointer = c.addressof(storage) + 32
                initial = storage.raw
                result = constructor(pointer, marker)
                constructed = storage.raw
                handle = Q.from_address(pointer + 8).value
                if result != pointer or not handle:
                    raise ValueError('Original parser wrapper construction failed')
                module_path = c.create_unicode_buffer(32768)
                if not kernel.GetModuleFileNameW(handle, module_path, len(module_path)):
                    raise ValueError('Cannot identify loaded parser module')
                if Path(module_path.value).resolve() != (args.gpa / 'ShaderParse.dll').resolve():
                    raise ValueError('Original wrapper loaded an unexpected module')
                endpoints = []
                for index, name in enumerate(('Assemble', 'GetAsmParseError', 'Release')):
                    address = Q.from_address(pointer + 48 + index * 8).value
                    expected = kernel.GetProcAddress(handle, name.encode())
                    if not expected or address != expected:
                        raise ValueError('Original wrapper endpoint mismatch')
                    endpoints.append(dict(name=name, address=hex(address), rva=hex(address - handle)))
                capacity = Q.from_address(pointer + 40).value
                length = Q.from_address(pointer + 32).value
                if length > 32768 or capacity < length:
                    raise ValueError('Unbounded wrapper module-name string')
                text = c.string_at(Q.from_address(pointer + 16).value if capacity > 15 else pointer + 16, length)
                released = unload(pointer + 8)
                unloaded = storage.raw
                second_release = unload(pointer + 8)
                cleanup(pointer + 8)
                cleaned = storage.raw
                if not released or second_release or storage.raw[:32] != initial[:32] or storage.raw[-32:] != initial[-32:]:
                    raise ValueError('Original wrapper lifetime or guard mismatch')
                rows.append(dict(pattern=pattern, marker=hex(marker), pointer=hex(pointer), returned=hex(result),
                    module_base=hex(handle), module_path=module_path.value, module_name=text.decode('ascii'),
                    endpoints=endpoints, initial=initial.hex(), constructed=constructed.hex(),
                    unloaded=unloaded.hex(), cleaned=cleaned.hex(), released=released, second_release=second_release))
        kernel_handle = kernel.GetModuleHandleW('kernel32.dll')
        for name in ('GetTickCount', 'GetCurrentProcessId', 'FloraMissingResearchProcedure'):
            argument = Narrow(name)
            for handle in (0, kernel_handle):
                holder = c.create_string_buffer(40)
                Q.from_buffer(holder).value = handle
                Q.from_buffer(holder, 32).value = 15
                before_holder, before_argument = holder.raw, argument.storage.raw
                actual = resolve(holder, argument.storage) or 0
                expected = (kernel.GetProcAddress(handle, name.encode()) or 0) if handle else 0
                if actual != expected or holder.raw != before_holder or argument.storage.raw != before_argument:
                    raise ValueError('Original resolver differs from the Windows loader')
                resolutions.append(dict(name=name, handle=hex(handle), actual=hex(actual), expected=hex(expected),
                    holder_before=before_holder.hex(), holder_after=holder.raw.hex(),
                    argument_before=before_argument.hex(), argument_after=argument.storage.raw.hex()))
        singleton_types = {hex(rva): c.string_at(base + rva).decode('ascii') for rva in (0x1fd240, 0x1fd268)}
        for rva, module in ((0xcea40, 0), (0xce640, base)):
            memory = c.create_string_buffer(b'\xa5'*104, 104)
            holder = c.addressof(memory)+32
            pointer = holder+8
            result = c.WINFUNCTYPE(P, P)(base+rva)(pointer)
            length = Q.from_address(pointer+16).value
            capacity = Q.from_address(pointer+24).value
            if result != pointer or not 0 < length <= capacity <= 32768:
                raise ValueError('Invalid original directory string')
            text = c.string_at(Q.from_address(pointer).value if capacity > 15 else pointer, length).decode('utf-8')
            module_path = c.create_unicode_buffer(32768)
            if not kernel.GetModuleFileNameW(module, module_path, len(module_path)):
                raise ValueError('Cannot read directory oracle')
            if Path(text).resolve() != Path(module_path.value).parent.resolve():
                raise ValueError('Original directory selection differs from Windows module identity')
            before_cleanup = memory.raw
            cleanup(holder)
            directories.append(dict(rva=hex(rva), module=hex(module), returned=hex(result), output=hex(pointer),
                text=text, module_path=module_path.value, before_cleanup=before_cleanup.hex(), cleaned=memory.raw.hex()))
        if kernel.GetModuleHandleW('shimloader64.dll'):
            raise ValueError('The direct resolver probe unexpectedly activated a shim')
    verify(args.gpa)
    report = dict(schema=1, completed=True, engine_recovered=False, shader_executed=False,
                  binary_sha256=PINNED, source_sha256=digest(Path(__file__)), rows=rows,
                  resolutions=resolutions, singleton_types=singleton_types, shim_active=False,
                  parser_preloaded_by_absolute_path=True, parser_base=hex(parser_dll._handle), directories=directories)
    (args.out / 'probe.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpa', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--child', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.gpa = args.gpa.resolve()
    if args.child:
        child(args)
        return
    verify(args.gpa)
    args.out.mkdir(parents=True, exist_ok=False)
    start = time.monotonic()
    timed_out = False
    with (args.out / 'process.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '--child',
            '--gpa', str(args.gpa), '--out', str(args.out.resolve())], stdout=log, stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            code = process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.kill()
            code = process.wait()
    manifest = dict(completed=code == 0 and not timed_out, exit_code=code, timed_out=timed_out,
                    elapsed_ms=round((time.monotonic()-start)*1000), deadline_seconds=30,
                    binary_sha256=PINNED, source_sha256=digest(Path(__file__)))
    (args.out / 'process.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    verify(args.gpa)
    if not manifest['completed']:
        raise SystemExit('Original module-route probe failed; see retained process.log')
    print(json.dumps(manifest))


if __name__ == '__main__':
    main()
