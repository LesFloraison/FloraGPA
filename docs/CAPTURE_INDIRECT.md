# GPU-generated DX11 indirect commands

Capture records `DispatchIndirect`, `DrawInstancedIndirect` and
`DrawIndexedInstancedIndirect` on immediate and deferred contexts. Each command
owns its argument-buffer identity and byte offset. Capture preserves the initial
buffer and preceding GPU mutations; the encoder emits the actual indirect record,
without replacing it with a direct command or freezing generated arguments.
Replay and Format source implementations are unchanged.

The capture-owned descriptor validator requires `DRAWINDIRECT_ARGS`, DWORD
alignment, and an in-bounds 12-, 16- or 20-byte argument range using widened
arithmetic. Missing resources, unexpected operands/payloads and malformed ranges
refuse publication. Deferred lowering retains pipeline and buffer lifetimes and
includes these commands in ordered UAV reset/KEEP dependencies. Nested and
repeated execution retain the same execution-time counter rules as direct work.
Existing predicate, SO and query-completion restrictions are not broadened.

The argument layouts follow Microsoft's
[DispatchIndirect](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-dispatchindirect),
[DrawInstancedIndirect](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-drawinstancedindirect) and
[DrawIndexedInstancedIndirect](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-drawindexedinstancedindirect)
contracts. Normalized IGPA v3 records are respectively `0x36`, `0x3d` and `0x3b`,
each with a 36-byte payload containing state, buffer reference and offset.

## Independent correctness evidence

The native fixture combines `CopyStructureCount` and a producer Dispatch to
write initially zero arguments at offset 16 in a guarded 128-byte raw buffer.
The consumer uses the resulting arguments through the original indirect API.
Graphics cases exercise nonzero vertex/index/instance starts, two instances and
signed base vertex -1. Compute cases update a structured buffer and hidden UAV
counter. Immediate, precompiled and nested twice-submitted lists are checked on
wrapped/unwrapped WARP and hardware, with both command-list restore modes.
All pixels, complete argument/result bytes, Debug Layer diagnostics and native
object lifetimes are checked against fixture-owned expectations.

The dual-player corpus includes two negative controls: point the indirect call
at zero arguments at offset 80, or remove only the indirect call. Both preserve
the original preview and must change executed output to the specified blue or
black image. Flora additionally exports command inventory, complete argument and
compute result buffers, selected counters, and every shader's DXBC/disassembly.
An export immediately after the first count copy, before the producer Dispatch,
must contain only the copied count and the original zero dimensions/guard words.
This separately detects replacing initial argument data with final GPU contents.
Eight malformed models per fixture must refuse without publishing a file.

An ignored development observer verifies actual original-player context slots
42, 40 and 39. Preserved reverse-engineering evidence and a local disassembly of
the hash-pinned DLL identify execute RVAs `0x27cf0`, `0x28260`, `0x28080` and Erg
context offset `0x20`. Public D3D11 staging readback checks all 128 argument bytes
before and after each indirect call; exact rendered output must survive this
observation. These private addresses are absent from production code.

## Startup injection and limits

Independent live-target modes 37, 38 and 39 generate arguments in a worker-built
command list, then execute the respective indirect API. The packaged controller
checks repeated captures, busy/cancel, explicit failure/recovery, disconnect and
continued target execution under a system-only child PATH. Actual loaded-module
inventories enforce package independence.

Measured totals, binary/frame hashes, previous-runtime control and raw evidence
manifest are in [capture-indirect-baseline.json](capture-indirect-baseline.json).
Capture CTest passes 149/149; the related full-application selection passes 51/51,
not the entire 299-test application suite. The 54 indirect files pass 1,114
dual-player checks, including the separate pre-producer resource check. Regression
adds 36 structure-count files / 832 checks and 24 OM counter files / 484 checks.
Three packaged startup launches pass 15 files / 97 checks. The prior runtime
refuses the same mode-37 program at deferred `DispatchIndirect`, publishes no
frame and permits normal target exit. Original-player observation checks 48
actual indirect calls and 96 complete argument-buffer boundaries.

All GPU checks run serially. This stage provides no new GF2, original GUI or
golden-frame acceptance; existing GF2 evidence remains tied to its recorded
runtime. Broader predicate/SO/query states, numeric `UINT_MAX` counter restoration,
extended UAV slots and arbitrary immediate-context synchronization remain open.
Overall capture delivery remains partial.
