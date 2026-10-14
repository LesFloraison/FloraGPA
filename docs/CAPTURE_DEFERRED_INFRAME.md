# Deferred compilation during capture

Deferred contexts now retain their compilation calls in the existing bounded
CPU journals while a frame is being recorded. They no longer enter the immediate
frame observer or invalidate a Present boundary merely because compilation occurs
on another thread. Map results, Unmap and query observations use the same routing.
Unknown context identities still reach the rejecting frame observer.

Only actual immediate `ExecuteCommandList` expands a finished journal into frame
events. First-use resource snapshots are taken at that execution position, and
earlier frame snapshots are retained. The implementation does not sort worker
timestamps or infer GPU order from compilation order. Unsubmitted lists contribute
no events; an executed unsupported journal still refuses the frame with its gap.
This follows Microsoft's [command list model](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-command-list).

Context kind and descriptor are immutable after wrapper publication. Routing reads
them without taking the mutable record mutex, so a foreign immediate caller does
not wait behind capture metadata before being rejected. A regression holds that
mutex while a foreign getter runs and joins; it must still return and report the
foreign-thread gap. External mutations and foreign immediate execution remain
rejected. There is no new Core, Replay, Encoder or shared-format dependency.

## Validation design

The extended native/wrapped buffer-discard fixture compiles two lists after frame
recording begins. The first frame compiles on the capture thread; the second uses
two worker threads, one per deferred context, then joins before execution. A/B/A
and B/A/B produce exact independent integer results 22 and 37, plus red and green
images. Native staging readback verifies resource bytes and pixels, restoration
flags and repeat provenance; the initial constant data remains the prior immediate
upload (17, 99), rather than either compiled version.

An additional Clear/query list is compiled and released without execution. It must
neither clear the output nor invalidate the frame. Executing a newly compiled
unsupported query list in a separate recording must fail. Source COM objects are
released before encoding, and Debug Layer warnings/errors fail the fixture.

Startup injection mode 29 creates a deferred context and compiles a mapped
constant-buffer Draw on a worker every frame. It joins before submission and
releases each context/list afterwards. Per-Present CPU colors provide an
independent oracle for repeated capture and both players. The existing controller
harness also checks cancellation, busy requests, explicit failure/recovery,
disconnect and target continuation.

Eight files pass **132** serial dual-player checks, including deletion of the
last discard and all Draws while preserving preview data. The shipping package
captures **five frames / 33 checks** with system-only child PATH and no loaded
Qt, Core/Replay, GPA, RenderDoc or Python modules. The same new target under the
previous stage's Runtime rejects its first requested frame at the worker Map,
publishes no file and exits normally, providing a before/after runtime control.

Both configurations build. The final complete capture-only run passes
**126/126**; the application's related deferred, stream, boundary and UI suites
pass **28/28**. The initial complete run had two obsolete stream expectations
requiring unsubmitted ClearState to reject; the corrected test requires no frame
events instead and retains foreign immediate rejection. A final status-text-only
update removes the obsolete in-frame-compilation gap, followed by **3/3**
bootstrap/controller/boundary checks and the shipping-package validation above.
This does not claim a complete 276-test application sweep.

Final run totals, package/module audits and binary hashes are recorded in
[capture-deferred-inframe-baseline.json](capture-deferred-inframe-baseline.json).
Raw outputs remain in the ignored `artifacts/capture-deferred-inframe-20261014`.
The initial fixture incorrectly expected native FinishCommandList with a null
output to succeed; it now receives and releases a real unsubmitted list. Failed
logs are preserved. No production behavior was changed to accommodate that error.

## Remaining scope

This validates same-frame compilation and synchronized worker submission for the
documented actions, not arbitrary application data races or all scheduling
patterns. Same-context compilation thread changes without proven synchronization,
foreign immediate execution, nested lists and unsupported hidden/query states
remain refused. Overlap between an immediate GPU use and a still-open deferred
Map still fails the mapped-resource guard. No new GF2, original GUI, golden-frame
or general GPU-correctness claim is made; earlier acceptance keeps its original
binary identity and historical Intel/NVIDIA pixel discrepancies remain open.
