"""Clone real original-player cache versions after replaying an unchanged capture.

Private-ABI development evidence only. The new versions are inspected, not
submitted as GPU workloads. Default-version playback before/after the clone is
retained as an interference control. No capture, installed DLL or COM object is
modified on disk. Run serially in isolated child processes.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import sys


def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('capture',type=Path)
    parser.add_argument('--reference-tools',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--reload-id',type=int,action='append',default=[],help='Recollect complete saved payload in version 7 after cloning')
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    sys.path.insert(0,str(args.reference_tools.resolve(strict=True)))
    from gpa_native import Wide, PLAYER_SHA256
    path=Path('C:/Program Files/IntelSWTools/GPA/dx11_player.dll')
    def digest(p):
        with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
    if digest(path)!=PLAYER_SHA256:raise ValueError('Unknown original player')
    report=dict(schema='FloraGPA original cache version clones 1',completed=False,passed=False,
                capture_sha256=digest(args.capture),player_sha256=PLAYER_SHA256,
                source_sha256=digest(Path(__file__)),default_playbacks=[],clones=[],
                cloned_version_workload_playback=False,cloned_version_resource_initialization=bool(args.reload_id),
                adapter_selection='Original default, undefined GpuId',callbacks=[])
    report['saved_payload_reloads']=[]
    report['observer_errors']=[];report['observers_restored']=True
    def save():(args.out/'versions.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    save()
    with os.add_dll_directory(str(path.parent)):
        dll=c.WinDLL(str(path));base=dll._handle;ptr,u32,u64=c.c_void_p,c.c_uint32,c.c_uint64
        fn=lambda rva,result,*params:c.WINFUNCTYPE(result,*params)(base+rva)
        def setting(key,value):
            a,b=Wide(key),Wide(value);fn(0x895b0,u64,ptr,ptr)(a.ptr,b.ptr)
        for key,value in [('no-gtquerymanager','true'),('single-threaded','true'),
                          ('no-first-playbacks','true'),('save-framebuffer','true')]:setting(key,value)
        (args.out/'open').mkdir()
        setting('save-filename',str((args.out/'open/framebuffer').resolve()))
        def tree(header):
            head=u64.from_address(header).value;count=u64.from_address(header+8).value
            if not head or count>100000:raise ValueError('Unbounded native tree')
            result=[];seen=set()
            def visit(node):
                if node==head:return
                if not node or node in seen or len(seen)>count or c.c_ubyte.from_address(node+25).value:
                    raise ValueError('Invalid native tree')
                seen.add(node);visit(u64.from_address(node).value);result.append(node);visit(u64.from_address(node+16).value)
            visit(u64.from_address(head+8).value)
            if len(result)!=count:raise ValueError('Native tree count differs')
            return result
        def snapshot(cache,version):
            graph=[]
            for kind,offset in [('state',0x68),('resource',0x78),('erg',0x58),('data',0x88)]:
                for entry in tree(cache+offset):
                    first,end=[u64.from_address(entry+i).value for i in [56,64]]
                    if not first or end<first or (end-first)%64 or (end-first)//64<=version:
                        raise ValueError('Missing native version')
                    node=first+version*64;obj=u64.from_address(node+48).value
                    graph.append(dict(id=u32.from_address(entry+32).value,kind=kind,
                        status=u32.from_address(node+40).value,
                        dependencies=[u32.from_address(n+28).value for n in tree(node+8)],
                        dependents=[u32.from_address(n+28).value for n in tree(node+24)],
                        object_identity=hex(obj),control_identity=hex(u64.from_address(node+56).value)))
            return graph
        def versions(cache):return [u32.from_address(n+28).value for n in tree(cache+8)]
        def reload(cache,ident,reference):
            # Observe actual node initializers; do not replace them with model
            # callbacks. All requested versions already exist, so these node
            # addresses remain stable throughout the bounded reload operation.
            identities={};slots={};callbacks=[];calls=[]
            protect=c.WinDLL('kernel32',use_last_error=True).VirtualProtect
            protect.argtypes=[ptr,c.c_size_t,u32,c.POINTER(u32)];protect.restype=c.c_int
            def write(address,value):
                previous=u32()
                if not protect(address,8,4,c.byref(previous)):raise c.WinError(c.get_last_error())
                u64.from_address(address).value=value;ignored=u32()
                if not protect(address,8,previous.value,c.byref(ignored)):raise c.WinError(c.get_last_error())
            for kind,offset in [('state',0x68),('resource',0x78),('erg',0x58),('data',0x88)]:
                for entry in tree(cache+offset):
                    first,end=[u64.from_address(entry+i).value for i in [56,64]]
                    for version,node in enumerate(range(first,end,64)):
                        identities[node]=(u32.from_address(entry+32).value,kind,version)
                        slot=u64.from_address(node).value+16
                        if slot not in slots:slots[slot]=u64.from_address(slot).value
            proto=c.WINFUNCTYPE(None,ptr,u32)
            def observer(address):
                original=proto(address)
                @proto
                def initialize(this,version):
                    index=len(calls)
                    try:
                        node_id,kind,expected=identities[this]
                        if version!=expected:raise ValueError('Initializer version differs from node storage')
                        calls.append(dict(id=node_id,kind=kind,version=version,
                                          initializer_rva=hex(address-base),before_status=u32.from_address(this+40).value))
                    except Exception as error:report['observer_errors'].append(str(error))
                    original(this,version)
                    if index<len(calls):calls[index]['after_status']=u32.from_address(this+40).value
                return initialize
            patched={}
            report['observers_restored']=False
            try:
                for slot,address in slots.items():
                    callback=observer(address);callbacks.append(callback)
                    replacement=c.cast(callback,ptr).value;write(slot,replacement);patched[slot]=(address,replacement)
                fn(0x9ca90,None,ptr,u32,u32,ptr)(cache,ident,7,reference)
            finally:
                for slot,(address,replacement) in reversed(list(patched.items())):
                    if u64.from_address(slot).value!=replacement:report['observer_errors'].append('Observer slot changed')
                    else:write(slot,address)
                report['observers_restored']=all(u64.from_address(slot).value==address for slot,(address,_) in patched.items())
            if report['observer_errors'] or not report['observers_restored']:raise ValueError('Incomplete native observer')
            return calls
        @c.WINFUNCTYPE(None,ptr,ptr)
        def error_callback(userdata,error):report['callbacks'].append(dict(userdata=hex(userdata or 0),error=hex(error or 0)))
        error=u64();filename=Wide(str(args.capture.resolve()));gpu=(u32*4)()
        try:
            status=fn(0x87c10,u64,*([u64]*8))(filename.ptr,0,0,0,c.addressof(gpu),0,0,c.addressof(error))
            report['open_status']=status;core=u64.from_address(base+0x208540).value
            if status or error.value or not core or not c.c_ubyte.from_address(core).value:raise ValueError('Original open failed')
            manager=u64.from_address(core+0x1b8).value
            cache=u64.from_address(manager+0x48).value+0x48
            def playback(label):
                folder=args.out/label;folder.mkdir();setting('save-filename',str((folder/'framebuffer').resolve()))
                arrays=[(u64*3)() for _ in range(3)];error.value=0
                from audit_replay import ReplayAudit
                monitor=ReplayAudit(base,core)
                try:
                    status=fn(0x87dc0,u64,*([u64]*10))(*[c.addressof(a) for a in arrays],0,0,0,0,0,
                        c.cast(error_callback,ptr).value,c.addressof(error))
                    audit=monitor.result()
                finally:monitor.close()
                report['default_playbacks'].append(dict(label=label,status=status,error=error.value,audit=audit));save()
                if status or error.value or report['callbacks']:raise ValueError('Original playback failed')
            playback('before')
            initial=snapshot(cache,0);report['initial_graph']=initial;report['initial_versions']=versions(cache);save()
            def clone(source,target):
                if target in versions(cache):raise ValueError('Target version already exists')
                before=snapshot(cache,source)
                fn(0x98450,None,ptr,u32,u32)(cache,source,target)
                source_after,target_after=snapshot(cache,source),snapshot(cache,target)
                unchanged=initial==snapshot(cache,0)
                row=dict(source=source,target=target,registered_versions=versions(cache),
                         source_graph=source_after,target_graph=target_after,default_unchanged=unchanged,
                         equal=before==source_after==target_after)
                report['clones'].append(row);save()
                if not row['equal'] or not unchanged or target not in row['registered_versions']:
                    raise ValueError('Native clone differs')
            for source,target in [(0,7),(7,9)]:clone(source,target)
            report['unregistered_hole_graph']=snapshot(cache,1)
            if any(n['status'] or n['dependencies'] or n['dependents'] or n['object_identity']!='0x0'
                   for n in report['unregistered_hole_graph']):raise ValueError('Allocated hole is not blank')
            if args.reload_id:
                sys.path.insert(0,str(args.reference_tools.resolve().parent/'standalone'))
                from frame import Frame
                with Frame(args.capture) as frame:
                    for ident in args.reload_id:
                        if ident not in frame.entries or frame.entries[ident].category not in [3,5,7,9]:
                            raise ValueError('Unknown saved reload identity')
                        raw=bytes(frame.payload(ident));backing=c.create_string_buffer(raw);start=c.addressof(backing)
                        vec=(u64*3)(start,start+len(raw),start+len(raw));reference=(u64*2)(c.addressof(vec),0)
                        before=snapshot(cache,7)
                        calls=reload(cache,ident,reference)
                        after=snapshot(cache,7)
                        row=dict(id=ident,category=frame.entries[ident].category,type=frame.entries[ident].type,
                                 payload_sha256=hashlib.sha256(raw).hexdigest(),before=before,after=after,
                                 initializers=calls,default_unchanged=snapshot(cache,0)==initial,
                                 other_clone_unchanged=snapshot(cache,9)==initial)
                        report['saved_payload_reloads'].append(row);save()
                        if not row['default_unchanged'] or not row['other_clone_unchanged']:
                            raise ValueError('Reload changed default/other clone metadata')
                clone(7,11)
            playback('after')
            report['default_graph_unchanged_after_playback']=snapshot(cache,0)==initial
            if not report['default_graph_unchanged_after_playback']:raise ValueError('Default graph changed')
        finally:
            fn(0x86fc0,u64)();report['closed']=u64.from_address(base+0x208540).value==0;save()
    from validate_corpus import original_rgba
    before=original_rgba(args.out/'before');after=original_rgba(args.out/'after')
    if not before or before!=after or not report['closed']:raise ValueError('Clone changed default output or failed close')
    report['default_execution_counts_equal']=report['default_playbacks'][0]['audit']==report['default_playbacks'][1]['audit']
    if not report['default_execution_counts_equal']:raise ValueError('Version operations changed default execution counts')
    report.update(completed=True,passed=True,default_pixels_equal=True,nodes=len(report['initial_graph']))
    save();print('Original version clones:',report['nodes'],'nodes, default pixels unchanged')


if __name__=='__main__':main()
