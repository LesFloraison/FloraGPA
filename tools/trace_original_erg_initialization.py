"""Research-only, process-local observation of the pinned GPA player's ERG initialization.

Run only on trusted development captures in an isolated child process. Virtual
table observers forward every call unchanged and restore slots before exit. No
DLL or capture is changed on disk, and no other process is inspected or injected.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import sys
import threading


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--reference-tools', type=Path, required=True)
    parser.add_argument('--gpa', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--no-observers', action='store_true', help='Original replay control with no virtual-table changes')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.reference_tools.resolve(strict=True)))
    from gpa_native import replay, PLAYER_SHA256
    path = args.gpa / 'dx11_player.dll'
    def digest(path):
        with path.open('rb') as stream:
            return hashlib.file_digest(stream, 'sha256').hexdigest()
    if digest(path) != PLAYER_SHA256:
        raise ValueError('Unrecognized player; refusing private ABI observation')
    report = dict(schema='FloraGPA original ERG initialization trace 1', completed=False,
                  capture_sha256=digest(args.capture), observers_enabled=not args.no_observers,
                  player_sha256=PLAYER_SHA256, observations=[], patches_restored=False,
                  scope='Process-local forwarding observers; not traditional-list GPU acceptance')
    def save():
        (args.out / 'trace.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    save()
    with os.add_dll_directory(str(args.gpa.resolve())):
        dll = c.WinDLL(str(path.resolve()))
        base = dll._handle
        ptr, u32, u64 = c.c_void_p, c.c_uint32, c.c_uint64
        protect = c.WinDLL('kernel32', use_last_error=True).VirtualProtect
        protect.argtypes = [ptr, c.c_size_t, u32, c.POINTER(u32)]
        protect.restype = c.c_int
        callbacks, patches = [], {}
        failures = []
        capture_stack = c.WinDLL('kernel32').RtlCaptureStackBackTrace
        capture_stack.argtypes = [u32, u32, c.POINTER(ptr), c.POINTER(u32)]
        capture_stack.restype = c.c_uint16
        stack_samples = []
        def write(address, value):
            old = u32()
            if not protect(address, 8, 4, c.byref(old)):
                raise c.WinError(c.get_last_error())
            u64.from_address(address).value = value
            ignored = u32()
            if not protect(address, 8, old.value, c.byref(ignored)):
                raise c.WinError(c.get_last_error())
        def attach(address, callback):
            previous = u64.from_address(address).value
            replacement = c.cast(callback, ptr).value
            callbacks.append(callback)
            write(address, replacement)
            patches[address] = (previous, replacement)
        def record(kind, **fields):
            report['observations'].append(dict(sequence=len(report['observations']), kind=kind,
                                               thread=threading.get_native_id(), **fields))
        def tree_nodes(header, limit=100000):
            head = u64.from_address(header).value
            count = u64.from_address(header + 8).value
            if not head or count > limit:
                raise ValueError('Invalid native tree header')
            nodes, seen = [], set()
            def visit(node):
                if node == head:
                    return
                if not node or node in seen or len(seen) >= limit or c.c_ubyte.from_address(node+25).value:
                    raise ValueError('Invalid native tree node')
                seen.add(node)
                visit(u64.from_address(node).value)
                nodes.append(node)
                visit(u64.from_address(node+16).value)
            visit(u64.from_address(head+8).value)
            if len(nodes) != count:
                raise ValueError('Native tree count mismatch')
            return nodes
        def snapshot_graph(erg):
            manager = u64.from_address(erg+0x18).value
            cache = u64.from_address(manager+0x48).value+0x48
            graph = []
            for kind, offset in [('data',0x88), ('resource',0x78), ('state',0x68), ('erg',0x58)]:
                for node in tree_nodes(cache+offset):
                    version = u64.from_address(node+56).value
                    end = u64.from_address(node+64).value
                    if not version or end < version+64 or (end-version)%64:
                        raise ValueError('Invalid version vector')
                    graph.append(dict(id=u32.from_address(node+32).value, kind=kind, version=0,
                                      allocated_versions=(end-version)//64,
                                      status=u32.from_address(version+0x28).value,
                                      node_vtable_rva=hex(u64.from_address(version).value-base),
                                      dependencies=[u32.from_address(p+28).value for p in tree_nodes(version+8)],
                                      dependents=[u32.from_address(p+28).value for p in tree_nodes(version+24)]))
            report['graph_before_first_erg_initialize'] = graph
        def observe_initializer(erg, wire_type):
            table = u64.from_address(erg).value
            slot = table + 5 * 8
            if slot in patches:
                return
            proto = c.WINFUNCTYPE(None, ptr)
            original = proto(u64.from_address(slot).value)
            rva = u64.from_address(slot).value - base
            @proto
            def initialize(this):
                event = u32.from_address(this + 0x10).value
                try:
                    record('initialize_enter', event=event, initializer_rva=hex(rva), vtable_rva=hex(table-base))
                    if 'graph_before_first_erg_initialize' not in report:
                        snapshot_graph(this)
                    if len(stack_samples) < 4:
                        addresses = (ptr * 64)()
                        count = capture_stack(0, 64, addresses, None)
                        stack_samples.append(dict(event=event, player_return_rvas=[hex(a-base) for a in addresses[:count]
                                                                                   if a and base <= a < base+0x300000]))
                except Exception as error:
                    failures.append(str(error))
                original(this)
                record('initialize_return', event=event, initializer_rva=hex(rva))
            attach(slot, initialize)
        factory_proto = c.WINFUNCTYPE(ptr, ptr, ptr, u32, c.c_uint16)
        factory_slot = base + 0x17ae40 + 10 * 8
        if u64.from_address(factory_slot).value != base + 0x3c550:
            raise ValueError('Unexpected ERG factory slot')
        original_factory = factory_proto(u64.from_address(factory_slot).value)
        @factory_proto
        def factory(this, shared_output, event, wire_type):
            result = original_factory(this, shared_output, event, wire_type)
            try:
                erg = u64.from_address(shared_output).value
                record('create', event=event, wire_type=wire_type, present=bool(erg))
                if erg:
                    if u32.from_address(erg + 0x10).value != event:
                        raise ValueError('Factory event layout changed')
                    observe_initializer(erg, wire_type)
            except Exception as error:
                failures.append(str(error))
            return result
        if not args.no_observers:
            attach(factory_slot, factory)
        try:
            report['replay'] = replay(args.capture.resolve(), args.gpa.resolve(), (args.out / 'framebuffer').resolve())
            if failures:
                raise ValueError('Observer errors: ' + '; '.join(failures))
            report['completed'] = True
        except Exception as error:
            report['error'] = str(error)
            raise
        finally:
            for address, (original, replacement) in reversed(list(patches.items())):
                if u64.from_address(address).value != replacement:
                    failures.append('Observer slot unexpectedly replaced')
                else:
                    write(address, original)
            report['patches_restored'] = all(u64.from_address(a).value == v[0] for a, v in patches.items())
            report['observer_errors'] = failures
            report['patched_slots'] = len(patches)
            report['initializer_stacks'] = stack_samples
            save()
    if failures or not report['patches_restored']:
        raise ValueError('Incomplete observer restoration or callback failure')
    print(json.dumps(dict(completed=report['completed'], observations=len(report['observations']),
                         patches_restored=report['patches_restored'])))


if __name__ == '__main__':
    main()
