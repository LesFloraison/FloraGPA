"""Observe the pinned GPA pixel-history request and its raw result vectors.

Research only. Private entry points never become product dependencies. A bounded
child preserves original files and records unknown status values without naming
them as fragment failure reasons.
"""
import argparse
import ctypes as c
import json
import os
from pathlib import Path
import subprocess
import sys

from probe_original_debugger_host import PLAYER_SHA256, P, Q, U, Wide, call_method, digest, modules, replay_adapter, select_gpu


def require(value, message):
    if not value:
        raise ValueError(message)


def child(args):
    binary = args.gpa.resolve() / 'dx11_player.dll'
    require(c.sizeof(P) == 8 and digest(binary) == PLAYER_SHA256, 'Pinned x64 player required')
    report = dict(schema=1, scope='original_event_pixel_history_raw_vectors', completed=False,
                  player_sha256=PLAYER_SHA256, capture_sha256=digest(args.capture),
                  source_sha256=digest(Path(__file__)), capture=str(args.capture.resolve()),
                  selection=dict(resource=args.resource, x=args.x, y=args.y, mip=args.mip, layer=args.layer),
                  event_filter=args.event, version=args.version, vendor=args.vendor,
                  requests=[], errors=[], callback_errors=[], playbacks=[])
    def save():
        (args.out/'history.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    save()
    with os.add_dll_directory(str(binary.parent)):
        dll = c.WinDLL(str(binary))
    base = dll._handle
    fn = lambda rva, result, *params: c.WINFUNCTYPE(result, *params)(base+rva)
    read = lambda address: Q.from_address(address).value
    @c.WINFUNCTYPE(None, P, P)
    def error_callback(user, error):
        report['errors'].append(dict(user=hex(user or 0), error=hex(error or 0)))
    def vector(pointer, width):
        start, end, capacity = (Q*3).from_address(pointer)
        require(start <= end <= capacity and (end-start) % width == 0 and capacity-start <= 64*1024*1024,
                'Invalid or unbounded original result vector')
        return c.string_at(start, end-start) if end != start else b''
    @c.WINFUNCTYPE(None, P, P, P, P, P)
    def result_callback(user, events, pixels, values, statuses):
        try:
            import struct
            raw = [vector(events,4), vector(pixels,1), vector(values,4), vector(statuses,1)]
            row = dict(user=hex(user or 0), events=[v[0] for v in struct.iter_unpack('<I',raw[0])],
                       pixel_bytes=raw[1].hex(), diagnostic_words=[v[0] for v in struct.iter_unpack('<I',raw[2])],
                       raw_status=list(raw[3]))
            report['requests'][-1]['results'].append(row)
        except Exception as error:
            report['callback_errors'].append(repr(error))
    error = Q()
    def playback(label):
        vectors = [(Q*3)() for _ in range(3)]
        error.value = 0
        status = fn(0x87dc0,U,*([P]*10))(*[c.addressof(v) for v in vectors],0,0,0,0,0,
                                              c.cast(error_callback,P),c.byref(error))
        report['playbacks'].append(dict(label=label,status=status,error=error.value))
        save()
        require(not status and not error.value and not report['errors'], 'Original replay failed')
    try:
        for key in ('single-threaded','no-gtquerymanager','no-first-playbacks'):
            name,value = Wide(key),Wide('true')
            fn(0x895b0,None,P,P)(name.ptr,value.ptr)
        enumeration,selected = select_gpu(fn,args.vendor)
        report.update(enumeration=enumeration, selected_gpu=selected)
        filename,gpu = Wide(str(args.capture.resolve())),(U*4)()
        gpu[:3] = selected['gpu_id']
        status = fn(0x87c10,U,*([P]*8))(filename.ptr,None,None,None,c.byref(gpu),None,None,c.byref(error))
        report['open'] = dict(status=status,error=error.value)
        save()
        host = read(base+0x208540)
        require(not status and not error.value and host, 'Original open failed')
        playback('before')
        actual = replay_adapter(base,host,args.capture)
        report['replay_adapter'] = actual
        require((actual['vendor'],actual['device'],actual['luid']) ==
                (selected['vendor'],selected['device'],gpu[1] | gpu[2]<<32), 'Actual adapter mismatch')
        manager = read(host+0x1b8)
        require(read(manager) == base+0x17ae40, 'Unexpected manager vtable')
        report['manager_vtable_rva'] = '0x17ae40'
        slot, original = base+0x17ae40+0x88, base+0x46d20
        require(read(slot) == original, 'Unexpected history dispatch function')
        prototype = c.WINFUNCTYPE(None,P,P,U,U,U,U)
        execute = prototype(original)
        def state():
            return dict(status=c.c_ubyte.from_address(manager+0x170).value,
                        diagnostic=U.from_address(manager+0x174).value,
                        event=U.from_address(manager+0x178).value)
        @prototype
        def observer(this, shared, version, mode, pass_index, flags):
            row = None
            try:
                erg = read(shared)
                event = call_method(erg,2,U)(erg)
                if mode in (1,2) and (not args.event or event in args.event):
                    require(len(report['requests'][-1]['dispatch']) < 10000, 'Dispatch evidence bound exceeded')
                    row = dict(event=event, mode=mode, pass_index=pass_index, flags=flags,
                               erg_vtable_rva=hex(read(erg)-base), before=state())
                    report['requests'][-1]['dispatch'].append(row)
            except Exception as error:
                report['callback_errors'].append(repr(error))
            execute(this,shared,version,mode,pass_index,flags)
            if row is not None:
                row['after'] = state()
        protect = c.WinDLL('kernel32',use_last_error=True).VirtualProtect
        protect.argtypes,protect.restype = [P,c.c_size_t,U,c.POINTER(U)],c.c_int
        def write_slot(value):
            old=U()
            if not protect(slot,8,4,c.byref(old)): raise c.WinError(c.get_last_error())
            Q.from_address(slot).value = value
            unused=U()
            if not protect(slot,8,old.value,c.byref(unused)): raise c.WinError(c.get_last_error())
        event_data = (U*len(args.event))(*args.event)
        begin = c.addressof(event_data) if args.event else 0
        events = (Q*3)(begin,begin+4*len(args.event),begin+4*len(args.event))
        resources = (Q*3)()
        for iteration in range(args.repeat):
            observed = args.observe and iteration > 0
            row = dict(iteration=iteration,results=[],phase='calling', observed=observed, dispatch=[])
            report['requests'].append(row)
            save()
            error.value = 0
            try:
                if observed: write_slot(c.cast(observer,P).value)
                status = fn(0x88d80,U,U,P,P,P,U,U,U,U,U,P,P,P)(
                    args.version,None,c.byref(events),c.byref(resources),args.resource,args.x,args.y,args.mip,args.layer,
                    c.cast(result_callback,P),c.cast(error_callback,P),c.byref(error))
            finally:
                if observed: write_slot(original)
                row['observer_restored'] = read(slot) == original
                save()
            row.update(phase='returned',status=status,error=error.value,
                       manager_status=c.c_ubyte.from_address(manager+0x170).value,
                       manager_diagnostic=U.from_address(manager+0x174).value,
                       manager_event=U.from_address(manager+0x178).value)
            save()
            require(not status and not error.value and not report['errors'] and not report['callback_errors'],
                    'Original history request failed')
            require(len(row['results']) == 1, 'Expected one synchronous result')
            require(row['observer_restored'], 'History observer was not restored')
            if iteration:
                require(row['results'] == report['requests'][0]['results'], 'Repeated history results differ')
        playback('after')
        report['replay_adapter_after'] = replay_adapter(base,host,args.capture)
        require(report['replay_adapter_after'] == actual, 'Replay adapter changed')
        report['modules'] = modules()
    finally:
        fn(0x86fc0,None)()
        report['closed'] = read(base+0x208540) == 0
        report['player_unchanged'] = digest(binary) == PLAYER_SHA256
        report['capture_unchanged'] = digest(args.capture) == report['capture_sha256']
        save()
    report['completed'] = report['closed'] and report['player_unchanged'] and report['capture_unchanged']
    save()
    require(report['completed'], 'Original history cleanup failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path)
    for name in ('resource','x','y'):
        parser.add_argument('--'+name,type=int,required=True)
    for name in ('mip','layer','version'):
        parser.add_argument('--'+name,type=int,default=0)
    parser.add_argument('--event',type=int,nargs='*',default=[])
    parser.add_argument('--vendor',type=int,choices=(4318,32902),required=True)
    parser.add_argument('--repeat',type=int,default=2)
    parser.add_argument('--observe',action='store_true',help='Forward selected dispatch calls after a control request')
    parser.add_argument('--gpa',type=Path,default=Path('C:/Program Files/IntelSWTools/GPA'))
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--timeout',type=float,default=120)
    parser.add_argument('--child',action='store_true',help=argparse.SUPPRESS)
    args=parser.parse_args()
    require(0 < args.repeat <= 10 and 0 < args.timeout <= 600, 'Invalid probe bounds')
    require(all(0 <= x <= 0xffffffff for x in [args.resource,args.x,args.y,args.mip,args.layer,args.version,*args.event]),
            'Expected uint32 selection values')
    if args.child:
        child(args)
        return
    args.out.mkdir(parents=True,exist_ok=False)
    command = [sys.executable,str(Path(__file__).resolve()),*sys.argv[1:],'--child']
    with (args.out/'process.log').open('w',encoding='utf-8') as log:
        process = subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            code = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            code = None
    (args.out/'process.json').write_text(json.dumps(dict(command=command,exit_code=code,timed_out=code is None,
                                                      timeout_seconds=args.timeout),indent=2)+'\n',encoding='utf-8')
    require(code == 0, 'Original history probe failed; inspect retained process and history reports')
    print(str((args.out/'history.json').resolve()))


if __name__ == '__main__':
    main()
