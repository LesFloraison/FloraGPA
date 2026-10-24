"""Probe pinned GPA debugger serialization with a bounded synthetic transport.

Research only: no capture, GPU, server, engine or product integration is exercised.
The parent owns a fresh child process and preserves failures and timeouts.
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

SHA256 = "0a531905386af6c5f2c0357d4179cec208bb171a287b262f3c4a5539d483bf5e"
PLAYER_SHA256 = "39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b"
P, Q, U = c.c_void_p, c.c_uint64, c.c_uint32


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def child(args):
    binary = args.gpa / "dx11_playback.dll"
    player_binary = args.gpa / "dx11_player.dll"
    require(c.sizeof(P) == 8 and digest(binary) == SHA256, "Pinned x64 playback is required")
    require(digest(player_binary) == PLAYER_SHA256, "Pinned player is required")
    report = dict(schema=1, scope="original_debugger_synthetic_wire", completed=False,
                  engine_recovered=False, shader_debug_parity=False, binary_sha256=SHA256,
                  player_sha256=PLAYER_SHA256, source_sha256=digest(Path(__file__)), cases=[])
    path = args.out / "wire.json"
    def save():
        path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    save()
    try:
        with os.add_dll_directory(str(args.gpa.resolve())):
            dll = c.WinDLL(str(binary.resolve()))
            player = c.WinDLL(str(player_binary.resolve()))
        base = dll._handle
        read = lambda address: Q.from_address(address).value
        dll.GetAPIFactory.restype = P
        factory = dll.GetAPIFactory()
        report["factory_rva"] = hex(factory - base)
        report["factory_vtable_rva"] = hex(read(factory) - base)
        report["factory_create_device_rva"] = hex(read(read(factory) + 8) - base)
        report["device_create_playback_rva"] = hex(read(base + 0x2b9098 + 0x18) - base)
        report["playback_debugger_rva"] = hex(read(base + 0x2b9e78 + 0x90) - base)
        require(report["factory_vtable_rva"] == "0x2b5058" and
                report["factory_create_device_rva"] == "0x49a20" and
                report["device_create_playback_rva"] == "0x4c950" and
                report["playback_debugger_rva"] == "0x5f2e0", "Unexpected original dispatch tables")

        received, sent, reads, writes, errors = bytearray(), bytearray(), [], [], []
        transfer = c.CFUNCTYPE(Q, P, P, Q)
        @transfer
        def receive(this, destination, count):
            if count > 4096 or count > len(received):
                errors.append("Synthetic input overrun")
                return 0
            reads.append(count)
            data = bytes(received[:count])
            del received[:count]
            c.memmove(destination, data, count)
            return count
        @transfer
        def send(this, source, count):
            if count > 4096 or len(sent) + count > 4096:
                errors.append("Synthetic output bound exceeded")
                return 0
            writes.append(count)
            sent.extend(c.string_at(source, count))
            return count
        table = (P * 2)(c.cast(receive, P), c.cast(send, P))
        transport = (P * 1)(c.addressof(table))
        codec = (Q * 3)(0, c.addressof(transport), 0)
        opcode = c.CFUNCTYPE(None, P, U)(base + 0x6be00)
        request = c.CFUNCTYPE(None, P, P)(base + 0x264ff0)
        status = c.CFUNCTYPE(None, P, P)(base + 0x263d30)
        result = c.CFUNCTYPE(None, P, P)(base + 0x262690)
        player_request = c.CFUNCTYPE(None, P, P)(player._handle + 0x10ba30)
        player_result = c.CFUNCTYPE(None, P, P)(player._handle + 0x10dd30)
        words = [0x10203040 + i for i in range(8)]
        selection = (U * 8)(*words)
        opcode(c.addressof(codec), 9)
        request(c.addressof(codec), c.addressof(selection))
        indices = [0, 1, 2, 3, 4, 5, 6, 2, 3, 4, 5, 2, 3, 4, 5, 6, 7]
        expected = struct.pack("<18I", 9, *(words[i] for i in indices))
        require(bytes(sent) == expected and writes == [4] * 18 and not errors,
                "Original request serialization mismatch")
        report["cases"].append(dict(name="request", original_words=words, field_indices=indices,
                                    wire_hex=sent.hex(), write_sizes=writes.copy(), matched=True))
        received.extend(sent[4:])
        decoded = (U * 8)()
        reads.clear()
        player_request(c.addressof(codec), c.addressof(decoded))
        require(list(decoded) == words and not received and not errors,
                "Original player request roundtrip mismatch")
        report["cases"].append(dict(name="cross_module_request", decoded_words=list(decoded),
                                    read_sizes=reads.copy(), matched=True))
        distinct = [0x55660000 + i for i in range(17)]
        received.extend(struct.pack("<17I", *distinct))
        player_request(c.addressof(codec), c.addressof(decoded))
        expected_words = [0] * 8
        for index, value in zip(indices, distinct):
            expected_words[index] = value
        require(list(decoded) == expected_words and not received and not errors,
                "Original repeated-field decoding mismatch")
        report["cases"].append(dict(name="repeated_field_decoding", wire_words=distinct,
                                    decoded_words=list(decoded), matched=True))
        for markers, expected_status in [(b"\0", 0), (b"\x01\x7f\0", 1)]:
            received.extend(markers + bytes([expected_status]))
            reads.clear()
            value = c.c_uint8(0xff)
            status(c.addressof(codec), c.addressof(value))
            require(value.value == expected_status and not received and not errors,
                    "Original response status mismatch")
            report["cases"].append(dict(name="status", markers_hex=markers.hex(),
                                        status=value.value, read_sizes=reads.copy(), matched=True))
        for first, second in [(b"", b""), (b"assembly", b"source")]:
            # Initialize only empty MSVC containers. Short strings remain inline;
            # this probe does not exercise original heap ownership or trace records.
            storage = (c.c_uint8 * 0xc0)()
            for offset in [0x68, 0xa0]:
                Q.from_buffer(storage, offset + 24).value = 15
            expected_result = (struct.pack("<3Q", 0, 0, 0) + struct.pack("<Q", len(first)) + first +
                               struct.pack("<2Q", 0, len(second)) + second)
            received.extend(expected_result)
            reads.clear()
            result(c.addressof(codec), c.addressof(storage))
            strings = []
            for offset in [0x68, 0xa0]:
                length = Q.from_buffer(storage, offset + 16).value
                require(length <= 15 and Q.from_buffer(storage, offset + 24).value == 15,
                        "Unexpected heap-backed result string")
                strings.append(bytes(storage[offset:offset + length]))
            require(strings == [first, second] and not received and not errors,
                    "Original result field mismatch")
            require(bytes(storage[:0x68]) == bytes(0x68) and
                    bytes(storage[0x88:0xa0]) == bytes(24), "Empty result containers changed")
            report["cases"].append(dict(name="empty_vectors_and_inline_strings",
                                        strings=[s.decode("ascii") for s in strings],
                                        read_sizes=reads.copy(), matched=True))
            sent.clear()
            writes.clear()
            player_result(c.addressof(codec), c.addressof(storage))
            require(bytes(sent) == expected_result and not errors, "Original result roundtrip mismatch")
            report["cases"].append(dict(name="cross_module_result", wire_hex=sent.hex(),
                                        write_sizes=writes.copy(), matched=True))
        report["binary_unchanged"] = digest(binary) == SHA256
        report["player_unchanged"] = digest(player_binary) == PLAYER_SHA256
        require(report["binary_unchanged"] and report["player_unchanged"], "Original binary changed")
        report["completed"] = True
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        save()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gpa", type=Path, default=Path("C:/Program Files/IntelSWTools/GPA"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--child", action="store_true")
    args = parser.parse_args()
    if args.child:
        child(args)
        return
    args.out.mkdir(parents=True, exist_ok=False)
    command = [sys.executable, str(Path(__file__).resolve()), "--gpa", str(args.gpa.resolve()),
               "--out", str(args.out.resolve()), "--child"]
    with (args.out / "child.log").open("w", encoding="utf-8") as log:
        try:
            process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=30,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            code = process.returncode
        except subprocess.TimeoutExpired:
            code = None
    report = dict(command=command, exit_code=code, timed_out=code is None, timeout_seconds=30)
    (args.out / "process.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    require(code == 0, "Original wire probe failed; inspect retained process and child reports")
    require(json.loads((args.out / "wire.json").read_text())["completed"], "Incomplete wire report")
    print("Nine original wire cases passed; no engine execution or shader trace parity claimed")


if __name__ == "__main__":
    main()
