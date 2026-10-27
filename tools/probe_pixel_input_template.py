"""Compile the owned pixel record template and record packed system declarations (Windows)."""
import argparse
import ctypes as c
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] /
                        'src/application/DxbcPixelInputs.cpp')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    owner = args.source.read_bytes()
    body = owner.decode('utf-8').split('const char *body = R"(', 1)[1].split(')";', 1)[0]
    source = '''#define RECORD_MODE 1
#define SELECT_X 3u
#define SELECT_Y 2u
#define SELECT_SAMPLE 3u
#define SELECT_LAYER 0u
#define MAX_RECORDS 1u
#define SAMPLE_MATCH (sample==SELECT_SAMPLE)
#define SAMPLE_ARGUMENT , uint sample:SV_SampleIndex
#define SAMPLE_VALUE sample
#define LAYER_ARGUMENT
#define LAYER_VALUE 0u
''' + body
    raw = source.encode('utf-8')
    (args.out / 'template.hlsl').write_bytes(raw)
    dll = c.WinDLL('d3dcompiler_47.dll')
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

    code, errors = c.c_void_p(), c.c_void_p()
    # Match compileHlsl's normal optimized mode.
    flags = 2048
    hr = dll.D3DCompile(raw, len(raw), b'Flora pixel input transport', None, None,
                        b'main', b'ps_5_0', flags, 0, c.byref(code), c.byref(errors))
    if errors.value:
        (args.out / 'compiler.txt').write_bytes(consume(errors))
    if hr < 0:
        raise RuntimeError(f'Template compilation failed: {hr:#x}')
    bytecode = consume(code)
    (args.out / 'template.dxbc').write_bytes(bytecode)
    assembly = c.c_void_p()
    if dll.D3DDisassemble(bytecode, len(bytecode), 0, None, c.byref(assembly)) < 0:
        raise RuntimeError('Template disassembly failed')
    text = consume(assembly).rstrip(b'\0').decode('utf-8')
    (args.out / 'template.asm').write_text(text, encoding='utf-8')
    report = dict(schema=1, scope='owned_pixel_record_template', compiler_flags=flags,
                  owner_sha256=hashlib.sha256(owner).hexdigest(),
                  hlsl_sha256=hashlib.sha256(raw).hexdigest(),
                  dxbc_sha256=hashlib.sha256(bytecode).hexdigest(),
                  declarations=[line for line in text.splitlines() if line.startswith('dcl_input')])
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
