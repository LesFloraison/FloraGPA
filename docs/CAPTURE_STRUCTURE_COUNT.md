# DX11 structure-count copies

Capture now records application `CopyStructureCount` calls on immediate and
deferred contexts and emits the existing normalized IGPA v3 `0x3f` record.
The destination buffer, aligned byte offset and source UAV identity are owned
CPU values. Replay and Format implementations are unchanged.

A copy consumes the source UAV's hidden count even when that view is unbound.
Immediate capture reads the live source value before forwarding the call.
Deferred lowering follows ordered resets and KEEP, including a reset with no
intervening Draw/Dispatch. A first KEEP requests the actual native view's value
at immediate submission. Nested lists share that counter history; each root
submission starts a new execution-time snapshot. Copies also invalidate any
CPU Map baseline for their destination.

The encoder validates source/destination kinds, structured counter descriptors,
element ranges, offset alignment/bounds, required initial bytes and counter
provenance. A required reset uses the previously verified temporary CS helper,
with a labelled ClearState/reset/unbind group. The following copy does not need
graphics bindings; subsequent actions restore their complete observed state.
Numeric `UINT_MAX`, predicated copies and same-resource copies remain explicit
refusals. Query-completion dependency analysis is not broadened by this change.

The [Microsoft API contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copystructurecount)
defines the hidden count source and 32-bit destination write.
[SetPredication](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-setpredication)
lists this operation among predicated commands. A local unwrapped Debug Layer
experiment additionally rejects structured destination buffers; the validator
now rejects that descriptor instead of accepting a silently ineffective copy.
The failed experiment and diagnostics remain in the ignored artifact directory.

## Correctness controls

The deterministic fixture covers Counter and Append views, immediate execution,
a deferred list, and nested repeated submissions with both restore modes. Two
distinct UAVs share one source buffer. Their compilation counts 9/11 become
execution counts 2/7. Five copies observe 2,3,5,7,8, including an unbound source
and reset-before-copy. Repeating the nested list produces 5,6,5,8,9.

Independent native checks compare every source/destination byte and all output
pixels on wrapped/unwrapped WARP and hardware. Destination guard words retain
`0xdeadbeef`; a shader consumes the copied counts to produce red or green.
Debug Layer warnings/errors and surviving native objects fail the fixture.
Ten malformed encoding cases per fixture must refuse without publishing a file.

The dual-player corpus includes a wrong-offset mutation that turns the output
blue and a final-Draw removal that leaves black. Both preserve the original
preview. Flora also exports both complete buffers at the last copy, each hidden
counter at its own view's last copy event, event inventory and three shaders
with DXBC/assembly. Its command-reference inspector does not report unrelated
unbound views as though they were referenced by the selected command.

An ignored development observer extends the preserved, hash-pinned original
player audit. Existing Ghidra evidence identifies CopyStructureCount execute RVA
`0x264b0`, context at Erg offset `0x20`, and native context slot 49. Public D3D11
readback compares the source count and complete source/destination buffers before
and after every application copy. Internal observer readbacks are excluded with
a reentrancy guard. Original output pixels must remain exact. None of this private
ABI code is linked into production.

## Delivery scope

Startup-injected mode 35 copies before and after a worker-compiled list; mode 36
places both copies inside that list. Both render from the copied values and
exercise repeated capture, busy/cancel, failure/recovery, disconnect and normal
target exit. The independent package uses a system-only child PATH and records
actual loaded modules. Regression and package totals, hashes, previous-runtime
control and raw evidence manifest are recorded in
[capture-structure-count-baseline.json](capture-structure-count-baseline.json).

The final capture suite passes 146/146 tests; the related application selection
passes 48/48, not a complete 296-test application sweep. The new corpus passes
36 files / 832 checks, and the CS/OM counter regression corpus passes another
48 files / 968 checks. Original-player observation verifies 240 application
copies and 480 before/after boundaries for each of the count, source buffer and
destination buffer. Both independent package launches pass 10 files / 65 checks
with no Qt, Core/Replay, GPA, RenderDoc or Python loaded by the target.
The prior runtime refuses mode 35 at `CopyStructureCount`, publishes no frame,
and leaves the target able to exit normally. All GPU checks run serially.

This is new count-copy coverage, not new GF2, original GUI or golden-frame
acceptance. Existing GF2 runtime identities and historical Intel/NVIDIA failures
remain unchanged. Indirect commands, broader predicate/SO/query state, numeric
`UINT_MAX` restoration and exhaustive API/state coverage remain incomplete.
Overall capture delivery stays partial.
