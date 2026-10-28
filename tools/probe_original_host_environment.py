"""Observe original relative parser loading without preloading ShaderParse.dll.

Run only in a bounded research child. This does not execute a shader or claim
that parser availability constructs a debugger engine.
"""
import argparse
import ctypes as c
import json
import os
from pathlib import Path
import sys

from probe_original_debugger_host import modules
from probe_original_module_routes import P, Q, verify


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpa', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    verify(args.gpa)
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.GetModuleHandleW.argtypes, kernel.GetModuleHandleW.restype = [c.c_wchar_p], P
    kernel.GetModuleFileNameW.argtypes = [P, c.c_wchar_p, c.c_uint32]
    kernel.GetModuleFileNameW.restype = c.c_uint32
    kernel.GetProcAddress.argtypes, kernel.GetProcAddress.restype = [P, c.c_char_p], P
    executable = c.create_unicode_buffer(32768)
    if not kernel.GetModuleFileNameW(None, executable, len(executable)):
        raise c.WinError(c.get_last_error())
    report = dict(schema='FloraGPA original host environment 1', completed=False,
                  executable=executable.value, python_executable=sys.executable,
                  python_home=sys.prefix, python_version=sys.version,
                  modules_before=modules(), directories=[], endpoints=[],
                  parser_preloaded=False, engine_recovered=False, shader_executed=False)
    try:
        if kernel.GetModuleHandleW('ShaderParse.dll'):
            raise ValueError('Parser was already loaded before the experiment')
        with os.add_dll_directory(str(args.gpa.resolve())):
            player = c.WinDLL(str((args.gpa / 'dx11_player.dll').resolve()))
            base = player._handle
            report['parser_loaded_by_player_import'] = bool(kernel.GetModuleHandleW('ShaderParse.dll'))
            if report['parser_loaded_by_player_import']:
                raise ValueError('Parser was imported before its original constructor')
            cleanup = c.WINFUNCTYPE(None, P)(base + 0x384d0)
            for label, rva in [('executable', 0xcea40), ('module', 0xce640)]:
                holder = c.create_string_buffer(40)
                pointer = c.addressof(holder)
                c.WINFUNCTYPE(P, P)(base + rva)(pointer + 8)
                length, capacity = Q.from_address(pointer + 24).value, Q.from_address(pointer + 32).value
                if length > 32768 or capacity < length:
                    raise ValueError('Invalid original directory string')
                value = c.string_at(Q.from_address(pointer + 8).value if capacity > 15 else pointer + 8, length)
                report['directories'].append(dict(kind=label, getter_rva=hex(rva), value=value.decode('utf-8')))
                cleanup(pointer)
            storage = c.create_string_buffer(b'\xa5' * 136, 136)
            pointer = c.addressof(storage) + 32
            report['initial'] = storage.raw.hex()
            returned = c.WINFUNCTYPE(P, P, Q)(base + 0xfac50)(pointer, 0x123456789abcdef0)
            report['constructed'] = storage.raw.hex()
            handle = Q.from_address(pointer + 8).value
            report.update(returned_self=returned == pointer, parser_loaded=bool(handle))
            if handle:
                name = c.create_unicode_buffer(32768)
                if not kernel.GetModuleFileNameW(handle, name, len(name)):
                    raise c.WinError(c.get_last_error())
                report['parser_path'] = name.value
                for index, symbol in enumerate(('Assemble', 'GetAsmParseError', 'Release')):
                    actual = Q.from_address(pointer + 48 + index * 8).value
                    expected = kernel.GetProcAddress(handle, symbol.encode())
                    if not expected or actual != expected:
                        raise ValueError('Original parser endpoint mismatch')
                    report['endpoints'].append(dict(name=symbol, rva=hex(actual - handle)))
            report['modules_constructed'] = modules()
            unload = c.WINFUNCTYPE(c.c_bool, P)(base + 0xc7f40)
            report['released'] = unload(pointer + 8)
            report['second_release'] = unload(pointer + 8)
            cleanup(pointer + 8)
            report['cleaned'] = storage.raw.hex()
            report['parser_loaded_after_release'] = bool(kernel.GetModuleHandleW('ShaderParse.dll'))
            if storage.raw[:32] != b'\xa5' * 32 or storage.raw[-32:] != b'\xa5' * 32:
                raise ValueError('Constructor guard changed')
            if not report['returned_self'] or report['released'] != bool(handle) or report['second_release']:
                raise ValueError('Unexpected original parser lifetime')
            report['modules_after'] = modules()
        verify(args.gpa)
        report['completed'] = True
    finally:
        args.out.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
