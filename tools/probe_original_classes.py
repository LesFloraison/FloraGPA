"""Process-local observation of the pinned development player; no file/shader edits."""
import argparse, ctypes as c, hashlib, json, struct, sys
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--frame', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--oracle-tools', type=Path, default=Path('D:/CDXrepo/FloraGPA/tools'))
a = p.parse_args()
out = a.out.resolve(); out.mkdir(parents=True, exist_ok=False)
sys.path[:0] = [str(a.oracle_tools), str(a.oracle_tools.parent/'standalone')]
from dx11 import method, P, U, I, Mapped, checked, Device
import audit_replay
from gpa_native import replay, PLAYER_SHA256

def u64(at): return c.c_uint64.from_address(at).value
def release(obj):
    if obj: method(obj, 2, U, [])(obj)

data = a.frame.read_bytes()
count = struct.unpack_from('<I', data, 12)[0]
table = struct.unpack_from('<Q', data, 0xf4)[0]
entries = [struct.unpack_from('<QQIBBH', data, table+i*24) for i in range(count)]
work = {e[0]: e[5] for e in entries if e[4] == 7 and e[5] in (0x35, 0x37)}

class Audit(audit_replay.ReplayAudit):
    def __init__(self, base, core):
        self.base = base; self.events = []; self.calls = []; self.callbacks = []; self.patches = {}
        self.failures = []; self.adapter = None; self.current = None
        self.protect = c.WinDLL('kernel32', use_last_error=True).VirtualProtect
        self.protect.argtypes = [P, c.c_size_t, U, c.POINTER(U)]; self.protect.restype = I
        manager = u64(core+0x1b8); address = u64(manager)+0x80
        context_ids = [e[0] for e in entries if e[4] == 5 and e[5] == 0x127]
        assert len(context_ids) == 1
        versions = c.WINFUNCTYPE(P,P,U)(base+0x9b350)(u64(manager+0x48)+0x48, context_ids[0])
        shared = c.WINFUNCTYPE(P,P,U)(u64(u64(versions)+0x10))(versions, 0)
        self.context = u64(u64(shared)+0x38)
        self.attach(self.context)
        proto = c.WINFUNCTYPE(None, P, P, U); original = proto(u64(address))
        @proto
        def dispatch(this, shared, version):
            erg = u64(shared); event = c.c_uint32.from_address(erg+0x10).value
            self.current = event
            self.events.append({'event': event, 'execute_rva': hex(u64(u64(erg)+0x70)-base)})
            self.attach(self.context)
            if event in work:
                try: self.attach(u64(erg+0x20))
                except Exception as error: self.failures.append(str(error))
            original(this, shared, version)
        self._patch(address, dispatch)

    def read_buffer(self, ctx, obj, label):
        device = P(); staging = P(); desc = (U*6)()
        method(obj, 3, None, [P])(obj, c.byref(device))
        try:
            if self.adapter is None:
                d = Device.__new__(Device); d.ptr = device.value
                self.adapter = d.adapter_info()
            method(obj, 10, None, [P])(obj, desc)
            assert 0 < desc[0] <= 4096
            size = desc[0]; desc[1:] = [3, 0, 0x20000, 0, 0]
            checked(method(device.value, 3, I, [P, P, P])(device.value, desc, None, c.byref(staging)), 'Create staging')
            method(ctx, 47, None, [P, P])(ctx, staging, obj)
            mapped = Mapped()
            checked(method(ctx, 14, I, [P,U,U,U,P])(ctx, staging, 0, 1, 0, c.byref(mapped)), 'Map')
            try: raw = c.string_at(mapped.data, size)
            finally: method(ctx, 15, None, [P,U])(ctx, staging, 0)
            name = f'{self.current}-{len(self.calls)}-{label}.bin'; (out/name).write_bytes(raw)
            return {'path': name, 'sha256': hashlib.sha256(raw).hexdigest(), 'uints': list(struct.unpack('<'+'I'*(size//4), raw))}
        finally:
            release(staging.value); release(device.value)

    def view_bytes(self, ctx, getter, label):
        view = P(); resource = P()
        method(ctx, getter, None, [U,U,P])(ctx, 0, 1, c.byref(view))
        try:
            if not view.value: return None
            method(view.value, 7, None, [P])(view.value, c.byref(resource))
            return self.read_buffer(ctx, resource.value, label)
        finally: release(resource.value); release(view.value)

    def binding(self, ctx):
        shader = P(); instances = (P*256)(); n = U(256)
        method(ctx, 107, None, [P,P,P])(ctx, c.byref(shader), instances, c.byref(n))
        assert n.value <= 256
        result = {'shader': hex(shader.value or 0), 'class_count': n.value, 'classes': []}
        try:
            for instance in instances[:n.value]:
                if not instance:
                    result['classes'].append(None); continue
                desc = (U*8)(); owner = P()
                method(instance, 8, None, [P])(instance, desc)
                method(instance, 7, None, [P])(instance, c.byref(owner))
                record = {'desc': list(desc), 'linkage': hex(owner.value or 0)}
                release(owner.value)
                for slot, key in [(9,'instance_name'), (10,'type_name')]:
                    buf = c.create_string_buffer(256); length = c.c_size_t(256)
                    method(instance, slot, None, [P,P])(instance, buf, c.byref(length))
                    record[key] = buf.value.decode('ascii', errors='replace')
                result['classes'].append(record)
            buffers = (P*3)()
            method(ctx, 109, None, [U,U,P])(ctx, 0, 3, buffers)
            try:
                result['constant_buffers'] = [self.read_buffer(ctx, obj, f'cb{i}') if obj else None for i,obj in enumerate(buffers)]
            finally:
                for obj in buffers: release(obj)
        finally:
            release(shader.value)
            for obj in instances[:n.value]: release(obj)
        return result

    def attach(self, ctx):
        address = u64(ctx)+69*8
        if address not in self.patches or u64(address) != self.patches[address][1]:
            proto_set = c.WINFUNCTYPE(None, P, P, P, U); original_set = proto_set(u64(address))
            @proto_set
            def set_shader(this, shader, instances, count):
                self.calls.append({'last_dispatched_event': self.current, 'name': 'CSSetShader', 'shader': hex(shader or 0),
                                   'class_count': count, 'classes': [hex(u64(instances+i*8)) for i in range(min(count, 256))] if instances else []})
                original_set(this, shader, instances, count)
            self._patch(address, set_shader)
        for slot, name, arguments in [(41,'Dispatch',[U,U,U]), (13,'Draw',[U,U])]:
            address = u64(ctx)+slot*8
            if address in self.patches and u64(address) == self.patches[address][1]: continue
            proto = c.WINFUNCTYPE(None, P, *arguments); original = proto(u64(address))
            def make_callback(original, name, proto):
                @proto
                def call(this, *args):
                    row = {'event': self.current, 'name': name, 'arguments': list(args)}; self.calls.append(row)
                    try:
                        if name == 'Dispatch': row['binding'] = self.binding(this); row['before'] = self.view_bytes(this, 106, 'uav-before')
                        else: row['ps_srv0'] = self.view_bytes(this, 73, 'ps-srv0')
                    except Exception as error: self.failures.append(str(error))
                    original(this, *args)
                    if name == 'Dispatch':
                        try: row['after'] = self.view_bytes(this, 106, 'uav-after')
                        except Exception as error: self.failures.append(str(error))
                return call
            self._patch(address, make_callback(original, name, proto))

    def result(self): return {'events': self.events, 'calls': self.calls, 'adapter': self.adapter, 'failures': self.failures}
    def close(self):
        saved = dict(self.patches)
        super().close()
        self.restored = all(u64(at) == before for at,(before,_) in saved.items())
        (out/'restoration.json').write_text(json.dumps({'hooks_restored': self.restored}))

audit_replay.ReplayAudit = Audit
failures = []
sys.unraisablehook = lambda error: failures.append(str(error.exc_value))
report = replay(a.frame.resolve(), Path('C:/Program Files/IntelSWTools/GPA'), out/'framebuffer', True)
report.update(frame_sha256=hashlib.sha256(data).hexdigest(), research_only=True, callback_failures=failures)
report['observer_sources'] = {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in
    [Path(__file__), a.oracle_tools/'gpa_native.py', a.oracle_tools/'audit_replay.py', a.oracle_tools.parent/'standalone/dx11.py']}
assert report['player_sha256'] == PLAYER_SHA256
report['player_unchanged'] = hashlib.sha256(Path(report['player_dll']).read_bytes()).hexdigest() == PLAYER_SHA256
(out/'observation.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
