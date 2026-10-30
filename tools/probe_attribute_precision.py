"""Compile owned attribute evaluation precision fixtures and retain DXBC/assembly."""
import argparse
import ctypes as c
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    dll = c.WinDLL('d3dcompiler_47.dll')
    kernel = c.WinDLL('kernel32.dll')
    kernel.GetModuleFileNameW.argtypes = [c.c_void_p, c.c_wchar_p, c.c_uint]
    kernel.GetModuleFileNameW.restype = c.c_uint
    buffer = c.create_unicode_buffer(32768)
    length = kernel.GetModuleFileNameW(dll._handle, buffer, len(buffer))
    if not length or length >= len(buffer):
        raise RuntimeError('Cannot identify the loaded compiler')
    path = Path(buffer.value)
    (args.out / 'compiler-identity.json').write_text(json.dumps(
        {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}, indent=2) + '\n', encoding='utf-8')
    dll.D3DCompile.argtypes = [c.c_void_p, c.c_size_t, c.c_char_p, c.c_void_p, c.c_void_p,
                              c.c_char_p, c.c_char_p, c.c_uint, c.c_uint,
                              c.POINTER(c.c_void_p), c.POINTER(c.c_void_p)]
    dll.D3DCompile.restype = c.c_long
    dll.D3DDisassemble.argtypes = [c.c_void_p, c.c_size_t, c.c_uint, c.c_char_p, c.POINTER(c.c_void_p)]
    dll.D3DDisassemble.restype = c.c_long
    def consume(blob):
        table = c.cast(blob, c.POINTER(c.POINTER(c.c_void_p))).contents
        pointer = c.WINFUNCTYPE(c.c_void_p, c.c_void_p)(table[3])(blob)
        size = c.WINFUNCTYPE(c.c_size_t, c.c_void_p)(table[4])(blob)
        data = c.string_at(pointer, size)
        c.WINFUNCTYPE(c.c_ulong, c.c_void_p)(table[2])(blob)
        return data
    reports = []
    for op, intrinsic, argument in (('centroid', 'EvaluateAttributeCentroid', ''),
                                     ('sample', 'EvaluateAttributeAtSample', ',index'),
                                     ('snapped', 'EvaluateAttributeSnapped', ',offset')):
        for precision in ('float', 'min16float', 'min10float'):
            for location in ('full', 'minimum'):
                name = f'{op}-{precision}-{location}'
                source = (f'float4 main(linear {precision}4 a:DATA, float4 p:SV_Position):SV_Target{{'
                          f'{"min16uint" if location == "minimum" else "uint"} index=uint(p.x);'
                          f'{"min16int2" if location == "minimum" else "int2"} offset=int2(p.xy)*7;'
                          f'{precision}4 value={intrinsic}(a{argument});return float4(value);}}')
                raw = source.encode('utf-8')
                (args.out / (name + '.hlsl')).write_bytes(raw)
                code, errors = c.c_void_p(), c.c_void_p()
                hr = dll.D3DCompile(raw, len(raw), name.encode(), None, None, b'main', b'ps_5_0',
                                    2048, 0, c.byref(code), c.byref(errors))
                if errors.value:
                    (args.out / (name + '.compiler.txt')).write_bytes(consume(errors))
                report = {'case': name, 'hresult': hr, 'hlsl_sha256': hashlib.sha256(raw).hexdigest()}
                if hr >= 0:
                    bytecode = consume(code)
                    (args.out / (name + '.dxbc')).write_bytes(bytecode)
                    assembly = c.c_void_p()
                    if dll.D3DDisassemble(bytecode, len(bytecode), 0, None, c.byref(assembly)) < 0:
                        raise RuntimeError('Attribute fixture disassembly failed')
                    text = consume(assembly).rstrip(b'\0').decode('utf-8')
                    (args.out / (name + '.asm')).write_text(text, encoding='utf-8')
                    report.update(dxbc_sha256=hashlib.sha256(bytecode).hexdigest(),
                                  evaluation=[s for s in text.splitlines() if s.startswith('eval_')])
                reports.append(report)
    (args.out / 'report.json').write_text(json.dumps(reports, indent=2) + '\n', encoding='utf-8')
    for report in reports:
        print(report['case'], report.get('evaluation', report['hresult']))


if __name__ == '__main__':
    main()
