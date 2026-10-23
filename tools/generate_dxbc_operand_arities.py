"""Rebuild scalar DXBC operand-count facts from pinned Microsoft sources.

Inputs are research files downloaded explicitly by the developer. This script
does not access the network and is not part of the product build. No upstream
parser implementation or runtime is copied into the generated header.
"""
import argparse
import hashlib
from pathlib import Path
import re


SOURCES = {
    "shader_binary": {
        "url": "https://raw.githubusercontent.com/microsoft/DirectXShaderCompiler/"
               "3620b16697cf57313d7a3abdb7a1c3aa3838c35c/"
               "projects/dxilconv/lib/ShaderBinary/ShaderBinary.cpp",
        "sha256": "0558a390ce2de0d76fc072893f15501d36e39abf379ff0611ead3b515ca7b63d",
    },
    "token_format": {
        "url": "https://raw.githubusercontent.com/microsoft/DirectX-Headers/"
               "461ef5138bd30922b44fbad77c4e47662812d2d5/"
               "include/directx/D3D12TokenizedProgramFormat.hpp",
        "sha256": "5862ef7c8b819f0d02ba73563a9df4dc0e89f11fe59d8f598def39122273c626",
    },
}


def verified(path, kind):
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SOURCES[kind]["sha256"]:
        raise ValueError(f"{kind} does not match the pinned Microsoft source")
    return raw.decode("utf-8")


def generate(shader_binary, token_format):
    definition = re.search(r"typedef enum D3D10_SB_OPCODE_TYPE\s*\{(.*?)\}\s*D3D10_SB_OPCODE_TYPE",
                           token_format, re.S)
    if definition is None:
        raise ValueError("Missing opcode enumeration")
    body = re.sub(r"//[^\n]*|/\*.*?\*/", "", definition.group(1), flags=re.S)
    names = [name.strip() for name in body.split(",") if name.strip()]
    if len(set(names)) != len(names) or any(re.fullmatch(r"[A-Z0-9_]+", name) is None for name in names):
        raise ValueError("Opcode enumeration requires explicit review")
    entries = {}
    for name, mnemonic, count in re.findall(r'SET\(\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*(\d+)\s*,',
                                           shader_binary):
        # Declarations/custom-data have instruction-specific immediate layouts.
        # Reserved tokens are not accepted even when a compiler uses them internally.
        if "_DCL_" in name or "_RESERVED" in name or name.endswith("_CUSTOMDATA"):
            continue
        opcode = names.index(name)
        if opcode in entries:
            raise ValueError("Duplicate opcode metadata")
        entries[opcode] = int(count), mnemonic
    if len(entries) != 190 or max(entries) != 234:
        raise ValueError("Unexpected executable opcode coverage")
    lines = ["#pragma once", "#include <map>", "namespace flora::dxbc_detail {",
             "// Generated scalar format facts from pinned Microsoft DXBC metadata.",
             "// Reproduce with tools/generate_dxbc_operand_arities.py; see",
             "// docs/DX11_DXBC_FORMAT_RESEARCH.md for sources and independent compiler probes.",
             "// This is operand framing, not an instruction executor or declaration parser.",
             "inline const std::map<unsigned, unsigned> operandArities{"]
    lines.extend(f"    {{{opcode}, {count}}}, // {mnemonic}"
                 for opcode, (count, mnemonic) in sorted(entries.items()))
    lines.extend(["};", "} // namespace flora::dxbc_detail", ""])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shader-binary", type=Path, required=True)
    parser.add_argument("--token-format", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    result = generate(verified(args.shader_binary, "shader_binary"),
                      verified(args.token_format, "token_format"))
    if args.check:
        if args.out.read_text(encoding="utf-8") != result:
            raise SystemExit("Generated operand facts differ from the checked-in header")
    else:
        args.out.write_text(result, encoding="utf-8", newline="\n")
    print("Verified 190 Microsoft-derived scalar operand counts")


if __name__ == "__main__":
    main()
