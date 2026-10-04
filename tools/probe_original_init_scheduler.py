"""Bounded original dependency-scheduler experiments; no capture or GPU acceptance.

Uses the hash-pinned player's native maps, sets and version-node layout. Custom
leaf callbacks only record initialization; no COM object or DX11 call is faked.
All allocations live in this isolated research process and die with it.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import random

PLAYER = '39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b'


def predicted(spec, initial_ready):
    status = {n['id']: 2 if n['id'] in initial_ready else 1 for n in spec}
    nodes = {n['id']: n for n in spec}
    children = {n['id']: set() for n in spec}
    calls = []
    def initialize(ident):
        calls.append(ident)
        status[ident] = 2
    def advance(ident):
        ready = True
        for parent in sorted(nodes[ident]['dependencies']):
            ready &= status[parent] == 2
            children[parent].add(ident)
        if ready:
            initialize(ident)
        for child in sorted(children[ident]):
            if status[child] == 2:
                initialize(child)
            elif status[child] == 1:
                advance(child)
    for kind in [4, 2, 1, 3]:
        for ident in sorted(n['id'] for n in spec if n['kind'] == kind):
            advance(ident)
    return calls, status, children


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--player', type=Path, default=Path('C:/Program Files/IntelSWTools/GPA/dx11_player.dll'))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    if hashlib.sha256(args.player.read_bytes()).hexdigest() != PLAYER:
        raise ValueError('Unknown player')
    report = dict(schema='FloraGPA original initialization scheduler 1', completed=False,
                  player_sha256=PLAYER, gpu_execution=False, original_capture_acceptance=False,
                  source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), cases=[])
    def save():
        (args.out/'scheduler.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    save()
    specs = [
        ('forward_dependency', [dict(id=10, kind=3, dependencies=[20]), dict(id=20, kind=3, dependencies=[])], []),
        ('ready_forward_dependency', [dict(id=10, kind=3, dependencies=[20]), dict(id=20, kind=3, dependencies=[])], [20]),
        ('cross_category', [dict(id=10, kind=2, dependencies=[20]), dict(id=20, kind=1, dependencies=[]),
                            dict(id=30, kind=3, dependencies=[10])], []),
        ('diamond', [dict(id=1, kind=3, dependencies=[20,30]), dict(id=20, kind=3, dependencies=[40]),
                     dict(id=30, kind=3, dependencies=[40]), dict(id=40, kind=3, dependencies=[])], []),
    ]
    rng = random.Random(0x9a730)
    for case in range(24):
        ids = rng.sample(range(1, 2000), 12)
        nodes = [dict(id=ident, kind=rng.randrange(1,5),
                      dependencies=sorted(rng.sample(ids[:i], min(i, rng.randrange(4))))) for i,ident in enumerate(ids)]
        specs.append((f'dag_{case}', nodes, []))
    with os.add_dll_directory(str(args.player.parent.resolve())):
        dll = c.WinDLL(str(args.player.resolve()))
        base = dll._handle
        ptr, u32, u64 = c.c_void_p, c.c_uint32, c.c_uint64
        fn = lambda rva, result, *parameters: c.WINFUNCTYPE(result, *parameters)(base+rva)
        category = fn(0x97dc0, ptr, ptr, ptr)
        lookup = fn(0x9b730, ptr, ptr, u32, u32)
        descriptors = {kind: fn(rva,ptr,ptr,ptr) for kind,rva in
                       [(1,0x97c90),(2,0x97b60),(3,0x97a30),(4,0x97900)]}
        offsets = {1:0x68,2:0x78,3:0x58,4:0x88}
        insert_set = fn(0x96f80, None, ptr, ptr, ptr)
        initialize_all = fn(0x9a8f0, None, ptr, u32)
        register = fn(0x3a980, None, ptr, u32, u32)
        for name, spec, ready in specs:
            # Native helpers allocate their own map nodes. Leaf version nodes
            # and empty-set sentinels have explicit recovered layouts; their
            # callbacks never dereference a synthetic resource or COM object.
            cache = c.create_string_buffer(0xa0)
            address = c.addressof(cache)
            heads = []
            for offset in [0x48,0x58,0x68,0x78,0x88]:
                sentinel = c.create_string_buffer(0x50)
                head = c.addressof(sentinel)
                heads.append(sentinel)
                for field in [0,8,16]:
                    u64.from_address(head+field).value = head
                c.c_uint16.from_address(head+24).value = 0x101
                u64.from_address(address+offset).value = head
            identities = {}
            node_kinds = {n['id']:n['kind'] for n in spec}
            managers = []
            for version in [0,1]:
                manager = c.create_string_buffer(0x200)
                sentinel = c.create_string_buffer(64)
                heads += [manager,sentinel]
                head = c.addressof(sentinel)
                for field in [0,8,16]:
                    u64.from_address(head+field).value = head
                c.c_uint16.from_address(head+24).value = 0x101
                u64.from_address(c.addressof(manager)+0x1e0).value = head
                managers.append(c.addressof(manager))
            calls, errors = [], []
            state_proto = c.WINFUNCTYPE(u32, ptr)
            init_proto = c.WINFUNCTYPE(None, ptr, u32)
            @state_proto
            def state(this):
                return u32.from_address(this+0x28).value
            @state_proto
            def identity(this):
                return identities[this][0]
            @init_proto
            def initialize(this, version):
                try:
                    ident, expected_version = identities[this]
                    if version != expected_version:
                        errors.append('Wrong version callback')
                    calls.append((ident, version))
                    if node_kinds[ident] == 3:
                        register(managers[version], 600 + (ident%2)*100, ident)
                    u32.from_address(this+0x28).value = 2
                except Exception as error:
                    errors.append(str(error))
            vtable = (u64*4)(c.cast(state,ptr).value, base+0x980c0,
                            c.cast(initialize,ptr).value, c.cast(identity,ptr).value)
            for node in spec:
                ident = u32(node['id'])
                u32.from_address(category(address+0x48,c.byref(ident))).value = node['kind']
                descriptor = descriptors[node['kind']](address+offsets[node['kind']],c.byref(ident))
                storage = c.create_string_buffer(128)
                heads.append(storage)
                begin = c.addressof(storage)
                for field,value in [(16,begin),(24,begin+128),(32,begin+128)]:
                    u64.from_address(descriptor+field).value = value
                for version in [0,1]:
                    instance = lookup(address,node['id'],version)
                    if instance != begin+version*64:
                        raise ValueError('Version-node layout changed')
                    identities[instance] = (node['id'],version)
                    u64.from_address(instance).value = c.addressof(vtable)
                    u32.from_address(instance+0x28).value = 2 if node['id'] in ready else 1
                    for offset in [8,24]:
                        sentinel = c.create_string_buffer(32)
                        heads.append(sentinel)
                        head = c.addressof(sentinel)
                        for field in [0,8,16]:
                            u64.from_address(head+field).value = head
                        c.c_uint16.from_address(head+24).value = 0x101
                        u64.from_address(instance+offset).value = head
                    if node['dependencies']:
                        deps = (u32*len(node['dependencies']))(*node['dependencies'])
                        insert_set(instance+8,c.addressof(deps),c.addressof(deps)+c.sizeof(deps))
            expected, states, children = predicted(spec,set(ready))
            versions = []
            for version in [0,1]:
                calls.clear()
                initialize_all(address,version)
                actual = [ident for ident, v in calls]
                statuses = {node['id']:u32.from_address(lookup(address,node['id'],version)+0x28).value for node in spec}
                if errors or actual != expected or statuses != states or any(s!=2 for s in statuses.values()):
                    raise ValueError((name,version,errors,actual,expected,statuses,states))
                groups, expected_groups = {}, {}
                head = u64.from_address(managers[version]+0x1e0).value
                seen = set()
                def visit(node):
                    if node == head:
                        return
                    if not node or node in seen or len(seen)>len(spec):
                        raise ValueError('Malformed original registration tree')
                    seen.add(node)
                    visit(u64.from_address(node).value)
                    start, end = u64.from_address(node+40).value,u64.from_address(node+48).value
                    if end<start or (end-start)%4 or end-start>65536:
                        raise ValueError('Malformed original registration vector')
                    groups[str(u32.from_address(node+32).value)] = [u32.from_address(p).value for p in range(start,end,4)]
                    visit(u64.from_address(node+16).value)
                visit(u64.from_address(head+8).value)
                for ident in expected:
                    if node_kinds[ident] == 3:
                        expected_groups.setdefault(str(600+(ident%2)*100),[]).append(ident)
                if groups != expected_groups:
                    raise ValueError('Original scheduler-to-registration order changed')
                versions.append(dict(version=version, original_order=actual, expected_order=expected,
                                     original_registration_groups=groups,
                                     all_ready=True, repeated_ids=len(actual)!=len(set(actual))))
            report['cases'].append(dict(name=name, nodes=spec, initially_ready=ready, versions=versions))
            save()
    first, second = report['cases'][:2]
    if first['versions'][0]['original_order'] != [20,10] or second['versions'][0]['original_order'] != [10,20,10]:
        raise ValueError('Hand-derived order witnesses disagree')
    report.update(completed=True, passed=True, cases_checked=len(specs), version_schedules_checked=len(specs)*2)
    save()
    print(json.dumps(dict(passed=True,cases=len(specs),version_schedules=len(specs)*2)))


if __name__ == '__main__':
    main()
