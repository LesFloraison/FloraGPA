"""Inventory local GPA PE files and debugger string candidates without loading them."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def scan(path):
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"Not a PE image: {path}")
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"Invalid PE signature: {path}")
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    optional = struct.unpack_from("<H", data, pe + 20)[0]
    sections = []
    for i in range(count):
        offset = pe + 24 + optional + i * 40
        virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", data, offset + 8)
        sections.append((raw, raw_size, rva))
    def address(offset):
        for raw, size, rva in sections:
            if raw <= offset < raw + size:
                return hex(rva + offset - raw)
        return None
    candidates = []
    for encoding, pattern in [("ascii", rb"[\x20-\x7e]{6,}"),
                              ("utf-16-le", rb"(?:[\x20-\x7e]\x00){6,}")]:
        for match in re.finditer(pattern, data):
            text = match[0].decode(encoding)
            if not re.search(r"debugger|shader.?debug|debug.?shader", text, re.IGNORECASE):
                continue
            candidates.append(dict(file_offset=match.start(), rva=address(match.start()),
                                   encoding=encoding, text=text))
    return dict(path=path.as_posix(), bytes=len(data), machine=hex(machine),
                sha256=hashlib.sha256(data).hexdigest(), candidates=candidates)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("C:/Program Files/IntelSWTools/GPA"))
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    paths = sorted(p for p in args.root.rglob("*") if p.is_file() and p.suffix.lower() in [".dll", ".exe", ".pyd"])
    modules = [scan(p) for p in paths]
    report = dict(schema=1, scope="local_gpa_debugger_string_candidates", engine_recovered=False,
                  source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), modules=modules)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Scanned {len(modules)} PE files; {sum(bool(m['candidates']) for m in modules)} contain candidates")


if __name__ == "__main__":
    main()
