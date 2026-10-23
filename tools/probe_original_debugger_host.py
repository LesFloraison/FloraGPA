"""Observe the pinned GPA debugger dispatch in an isolated research process.

Uses only the Python standard library. The original DLL and capture stay read-only.
A process-local forwarding observer records the real request's host field; it never
supplies an engine. This is dispatch/lifecycle evidence, not shader-debug parity.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

PLAYER_SHA256 = "39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b"
P, U, Q = c.c_void_p, c.c_uint32, c.c_uint64


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class Wide:
    """Read-only MSVC x64 wstring argument; the callee must not own this storage."""
    def __init__(self, text):
        encoded = text.encode("utf-16-le")
        self.characters = c.create_string_buffer(encoded + b"\0\0")
        self.storage = (Q * 4)(c.addressof(self.characters), 0, len(encoded) // 2,
                               max(8, len(encoded) // 2))
        self.ptr = c.addressof(self.storage)


def modules():
    kernel = c.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = P
    kernel.K32EnumProcessModules.argtypes = [P, P, U, P]
    kernel.K32EnumProcessModules.restype = c.c_int
    kernel.GetModuleFileNameW.argtypes = [P, P, U]
    kernel.GetModuleFileNameW.restype = U
    handles, needed = (P * 2048)(), U()
    if not kernel.K32EnumProcessModules(kernel.GetCurrentProcess(), handles, c.sizeof(handles), c.byref(needed)):
        raise c.WinError(c.get_last_error())
    if needed.value > c.sizeof(handles):
        raise ValueError("Module snapshot exceeds its bound")
    result = []
    for handle in handles[:needed.value // c.sizeof(P)]:
        name = c.create_unicode_buffer(32768)
        length = kernel.GetModuleFileNameW(handle, name, len(name))
        if not 0 < length < len(name):
            raise c.WinError(c.get_last_error())
        result.append(name.value)
    return sorted(result)


def child(args):
    player = args.gpa.resolve() / "dx11_player.dll"
    if digest(player) != PLAYER_SHA256 or c.sizeof(P) != 8:
        raise ValueError("This private-ABI probe requires the pinned x64 player")
    report = dict(schema="FloraGPA original debugger dispatch 1", completed=False,
                  capture=str(args.capture.resolve()), capture_sha256=digest(args.capture),
                  player_sha256=PLAYER_SHA256, source_sha256=digest(Path(__file__)),
                  adapter_selection="Original default with undefined GpuId; no product adapter assertion",
                  event=args.event, snapshots=[], requests=[], playbacks=[], errors=[], observer_errors=[],
                  field_write_watch=False, engine_recovered=False, shader_debug_parity=False)
    def save():
        (args.out / "dispatch.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    save()
    with os.add_dll_directory(str(player.parent)):
        dll = c.WinDLL(str(player))
        base = dll._handle
        fn = lambda rva, result, *params: c.WINFUNCTYPE(result, *params)(base + rva)
        read = lambda address: Q.from_address(address).value
        def snapshot(label):
            host = read(base + 0x208540)
            engine = read(host + 0x3c0) if host else 0
            row = dict(label=label, host=hex(host), engine=hex(engine), modules=modules())
            if engine:
                table = read(engine)
                row.update(vtable=hex(table), entry=hex(read(table)))
            report["snapshots"].append(row)
            save()
            return host, engine
        def setting(key, value):
            a, b = Wide(key), Wide(value)
            fn(0x895b0, None, P, P)(a.ptr, b.ptr)
        settings = ["single-threaded"]
        if not args.original_initialization:
            settings += ["no-gtquerymanager", "no-first-playbacks"]
        report["settings"] = {key: "true" for key in settings}
        for key in settings:
            setting(key, "true")
        @c.WINFUNCTYPE(None, P, P)
        def error_callback(userdata, error):
            report["errors"].append(dict(userdata=hex(userdata or 0), error=hex(error or 0)))
        # The callback payload is deliberately not decoded without a proven layout.
        @c.WINFUNCTYPE(None, P, P, P)
        def result_callback(userdata, result, extra):
            report["requests"][-1]["result_callbacks"].append(
                dict(userdata=hex(userdata or 0), result=hex(result or 0), extra=hex(extra or 0)))
        error = Q()
        def playback(label):
            vectors = [(Q * 3)() for _ in range(3)]
            error.value = 0
            status = fn(0x87dc0, U, *([P] * 10))(
                *[c.addressof(value) for value in vectors], 0, 0, 0, 0, 0,
                c.cast(error_callback, P), c.byref(error))
            report["playbacks"].append(dict(label=label, status=status, error=hex(error.value)))
            save()
            if status or error.value or report["errors"]:
                raise ValueError("Original playback failed")
        try:
            snapshot("loaded")
            filename, gpu = Wide(str(args.capture.resolve())), (U * 4)()
            status = fn(0x87c10, U, *([P] * 8))(
                filename.ptr, None, None, None, c.byref(gpu), None, None, c.byref(error))
            report["open"] = dict(status=status, error=hex(error.value))
            host, _ = snapshot("opened")
            if status or error.value or not host or not c.c_ubyte.from_address(host).value:
                raise ValueError("Original open failed")
            playback("before_requests")
            snapshot("replayed")
            # The request carries eight raw uint32 selection words. The native
            # dispatcher compares word 1 to an ERG identity. Other word semantics
            # remain unproven and are retained verbatim, not named as shader fields.
            selection = (U * 8)(0, args.event, 0, 0, 0, 0, 0, 0)
            versions = (Q * 3)()
            slot, original = base + 0x183198, base + 0x902b0
            if read(slot) != original:
                raise ValueError("Unexpected debugger request vtable")
            prototype = c.WINFUNCTYPE(c.c_ubyte, P, P)
            execute = prototype(original)
            @prototype
            def observer(request, manager):
                try:
                    owner = read(manager + 0x98)
                    row = dict(request_vtable_rva=hex(read(request) - base),
                               host_matches_global=owner == read(base + 0x208540),
                               engine_before=hex(read(owner + 0x3c0)),
                               version=U.from_address(request + 0x10).value,
                               selection=list((U * 8).from_address(request + 0x20)))
                    report["requests"][-1]["execution"].append(row)
                    result = execute(request, manager)
                    row.update(returned=int(result), engine_after=hex(read(owner + 0x3c0)))
                    return result
                except Exception as error:
                    # Never let ctypes suppress an observer failure as a successful probe.
                    report["observer_errors"].append(repr(error))
                    return 1
            protect = c.WinDLL("kernel32", use_last_error=True).VirtualProtect
            protect.argtypes, protect.restype = [P, c.c_size_t, U, c.POINTER(U)], c.c_int
            def write_slot(value):
                old = U()
                if not protect(slot, 8, 4, c.byref(old)):
                    raise c.WinError(c.get_last_error())
                Q.from_address(slot).value = value
                unused = U()
                if not protect(slot, 8, old.value, c.byref(unused)):
                    raise c.WinError(c.get_last_error())
            for observed in (False, True):
                row = dict(observed=observed, result_callbacks=[], execution=[])
                report["requests"].append(row)
                callback = c.cast(observer, P).value
                try:
                    if observed:
                        write_slot(callback)
                    error.value = 0
                    status = fn(0x88950, U, P, P, U, P, P, P, P)(
                        c.byref(selection), c.byref(versions), 0, None,
                        c.cast(result_callback, P), c.cast(error_callback, P), c.byref(error))
                    row.update(status=status, error=hex(error.value))
                finally:
                    if observed:
                        write_slot(original)
                    row["observer_restored"] = read(slot) == original
                    save()
                if (status or error.value or report["errors"] or report["observer_errors"] or
                        not row["observer_restored"]):
                    raise ValueError("Original debugger request failed")
                snapshot("after_observed_request" if observed else "after_control_request")
            if len(report["requests"][1]["execution"]) != 1:
                raise ValueError("Expected one synchronous debugger request execution")
            playback("after_requests")
            snapshot("replayed_after_requests")
        finally:
            fn(0x86fc0, None)()
            snapshot("closed")
            report["closed"] = read(base + 0x208540) == 0
            report["player_unchanged"] = digest(player) == PLAYER_SHA256
            report["capture_unchanged"] = digest(args.capture) == report["capture_sha256"]
            save()
    report["completed"] = report["closed"] and report["player_unchanged"] and report["capture_unchanged"]
    save()
    if not report["completed"]:
        raise ValueError("Incomplete probe cleanup")
    print(json.dumps({key: report[key] for key in ("completed", "engine_recovered", "shader_debug_parity")}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--event", type=int, required=True)
    parser.add_argument("--gpa", type=Path, default=Path("C:/Program Files/IntelSWTools/GPA"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--original-initialization", action="store_true",
                        help="Retain original query-manager and initial-playback defaults")
    parser.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not 0 < args.event <= 0xffffffff or not 0 < args.timeout <= 600:
        parser.error("Expected uint32 event and a timeout of at most 600 seconds")
    if args.child:
        child(args)
        return
    args.out.mkdir(parents=True, exist_ok=False)
    command = [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], "--child"]
    with (args.out / "process.log").open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            code = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            code = None
    (args.out / "process.json").write_text(json.dumps(dict(exit_code=code, timed_out=code is None,
        timeout_seconds=args.timeout, command=command), indent=2) + "\n", encoding="utf-8")
    if code != 0:
        raise SystemExit("Original debugger probe failed; inspect process.log and dispatch.json")
    print(str((args.out / "dispatch.json").resolve()))


if __name__ == "__main__":
    main()
