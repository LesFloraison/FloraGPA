"""Synthetic engine receiver for the pinned original request, research only.

The original installation and executable code remain unchanged. A real opened
host receives a temporary child-owned callback object only during one request.
This establishes a caller contract, never the existence or behavior of an
original debugger engine. The ordinary host probe remains the entry point.
"""
import ctypes as c
import hashlib
from pathlib import Path

P, U, Q = c.c_void_p, c.c_uint32, c.c_uint64


class EngineContract:
    def __init__(self, base, report):
        self.base, self.report = base, report
        report['synthetic_contract'] = self.evidence = dict(
            source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            original_engine=False, calls=[], results=[], destructors=[], callback_errors=[], restored=False)
        self.request = None
        self.closing_owner = None

        @c.WINFUNCTYPE(P, P, P, P, U, P)
        def invoke(receiver, status, selection, version, output):
            try:
                self.evidence['calls'].append(dict(
                    receiver=receiver, status=status, selection_address=selection,
                    selection=list((U * 8).from_address(selection)), version=version,
                    output=output, output_before=c.string_at(output, 0xa0).hex(),
                    status_before=Q.from_address(status).value,
                    request=self.request, canonical_status=Q.from_address(base + 0x208560).value))
                # Preserve the original empty result. Copy its canonical status
                # identity without creating, interpreting or freeing a status.
                Q.from_address(status).value = Q.from_address(base + 0x208560).value
            except Exception as error:
                self.evidence['callback_errors'].append(repr(error))
            return status

        @c.WINFUNCTYPE(c.c_int, P, P)
        def result(userdata, output):
            try:
                self.evidence['results'].append(dict(
                    userdata=userdata, output=output, bytes=c.string_at(output, 0xa0).hex()))
            except Exception as error:
                self.evidence['callback_errors'].append(repr(error))
            return 0

        # Child-owned storage is deliberately not freed by the callback.
        @c.WINFUNCTYPE(None, P, U)
        def destructor(receiver, flags):
            if not self.closing_owner:
                self.evidence['callback_errors'].append('Destructor outside host close')
                return
            self.evidence['destructors'].append(dict(receiver=receiver, flags=flags,
                installed_engine=Q.from_address(self.closing_owner + 0x3c0).value))

        self.invoke, self.result_callback, self.destructor = invoke, result, destructor
        self.table = (Q * 2)(c.cast(invoke, P).value, c.cast(destructor, P).value)
        self.storage = c.create_string_buffer(bytes([0xa5] * 0x88), 0x88)
        self.receiver = c.addressof(self.storage) + 0x40
        Q.from_address(self.receiver).value = c.addressof(self.table)
        self.before = self.storage.raw
        self.evidence.update(receiver=self.receiver, storage_before=self.before.hex())

    def execute(self, execute, request, manager, owner):
        field = Q.from_address(owner + 0x3c0)
        if field.value:
            raise ValueError('Refusing to replace a non-null original engine')
        self.request = request
        self.evidence['request'] = dict(address=request, owner=owner, manager=manager,
            version=U.from_address(request + 0x10).value,
            selection=list((U * 8).from_address(request + 0x20)))
        field.value = self.receiver
        try:
            return execute(request, manager)
        finally:
            self.evidence['installed_field_preserved'] = field.value == self.receiver
            field.value = 0
            self.evidence['restored'] = field.value == 0
            self.evidence['storage_after'] = self.storage.raw.hex()
            if self.storage.raw != self.before or self.evidence['callback_errors']:
                raise ValueError('Synthetic receiver guard or callback failure')

    def close(self, owner, closer):
        if not owner:
            closer()
            return
        if Q.from_address(owner + 0x3c0).value:
            raise ValueError('Refusing to replace an original engine during close')
        self.closing_owner = owner
        Q.from_address(owner + 0x3c0).value = self.receiver
        closer()
        # The host may now be freed. Inspect only child-owned storage and the
        # module's singleton, never dereference the old host allocation.
        self.evidence['closed_singleton'] = Q.from_address(self.base + 0x208540).value
        self.evidence['storage_after_close'] = self.storage.raw.hex()
        self.closing_owner = None
