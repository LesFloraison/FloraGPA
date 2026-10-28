# Native DXBC resource information

This P6 checkpoint implements texture `resinfo` in the independent C++ executor.
The earlier real Helldivers event 893 probe stopped at opcode 61 before completing
any instruction. Resource queries and texture UAV execution are separate remaining
requirements; this checkpoint does not claim a complete production debugger.

## Query contract

Queries use the actual native resource descriptor and bound view. Width, height,
depth, array ranges and mip counts retain the selected view identity. Integer, floating-point and reciprocal modes
preserve register words, swizzles, destination masks and component validity.
Reciprocals apply to spatial dimensions, not array lengths or mip counts.

Missing metadata remains unknown. Verified unbound views produce the instruction's
null result; out-of-range mip selection preserves the actual adapter's query result. Unknown mip
inputs retain independent information such as that count. MinLOD-clamped dimensions
and the cube-array z return are not presented as defined values. These semantics
follow the public [resinfo contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/resinfo--sm4---asm-).
The pinned Microsoft token header defines return-mode bits 11 through 12. The
implementation does not import a RenderDoc interpreter or private GPA ABI.

## Metadata observation

`Replay::observeTextureMetadata` reads canonical native descriptors and MinLOD
without mapping texture pixels. `observeTextureStorage` reuses that description
and retains its existing non-MSAA storage and byte-budget restrictions. DXBC input
snapshots now retain descriptions separately from bytes, under source identity
`independent-replay-resource-inputs/4`.

MSAA snapshots still explicitly report unavailable pixel storage. Their native
descriptions can nevertheless support resource queries. The existing empty-storage
assertion remains; no resolve or invented sample data is introduced. View ranges
are checked against the native description before either path proceeds. Queries
never modify replay bindings/resources. Most queries execute from metadata alone;
3D UAV queries, reciprocal return modes and out-of-range mip selections use a
private same-adapter GPU operation as described below.

## Native query differences

A texture with base depth 10, mip 1, FirstWSlice 1 and WSize 3 exposes a
cross-driver discrepancy. Native `resinfo_uint` at mip zero returned depth 3 on
NVIDIA, 4 on Intel and 5 on WARP. The initial CPU view-depth formula failed on
Intel and WARP. The recorded native values are observations, not a new portable
formula or a declaration that any driver's behavior defines the specification.
The public [D3D11.3 specification, section 22.4.14](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
states view-relative dimensions; it does not justify choosing one observed
driver's value for all adapters.

The first complete matrix additionally exposed 63 NVIDIA out-of-range differences
and 20 Intel reciprocal differences (83 failures, 534 passes, zero skips). NVIDIA
returned nonzero dimensions where the public contract specifies an unbound-like
size result. Intel's reciprocal words differed by one ULP in the failing cases.
These failures are not removed or converted to undefined components. Native
execution words are retained without claiming that a driver overrides the public
contract. Ordinary valid integer/float queries still execute from metadata.

The executor therefore queries a private texture and identical view descriptor
on its existing isolated device, which verifies the actual adapter LUID. The
helper executes a project-owned `resinfo` shader in the requested return mode,
retains raw words, and records GPU assistance. Query results have a bounded
256-entry cache local to that device and keyed by the base/view descriptors and
return mode, dimension, resource kind, mip selector and MinLOD. Unknown mip inputs
remain unknown without dispatch; null bindings retain their specified results.
Cancellation is checked
even on cache hits, and submitted-operation failure requires device recreation.
The helper identity advances to `dxbc-gpu/8`. No vendor-name heuristic is used.

## Validation

The native comparison fixture executes project-assembled `resinfo` in a real CS
and writes its raw words to a private structured result buffer. The independent
executor consumes the observed metadata and compares all defined components
exactly. SRV cases also execute through the VS and PS lane paths; these are
comparisons against native CS query results, not full native VS/PS invocations.

The matrix uses R32_UINT views on WARP, NVIDIA and Intel; texture dimensions including MSAA and
cube arrays; SRVs and legal texture UAVs; all three return modes; view mip/array
subranges; out-of-range and null bindings; and MinLOD. Undefined components have
explicit expected validity masks and are not compared as deterministic values.
The native snapshots, resource preservation and D3D11 diagnostics are checked.

Six independent comparisons compile HLSL `GetDimensions` without replacing its
executable bytecode. They reproduce the 3D UAV subrange and out-of-range 2D SRV
results on all three adapters. The reciprocal-mode comparison continues to use
the directly assembled native instruction; an HLSL arithmetic reciprocal would
test a different instruction and is not substituted as its oracle.

The first smoke run failed six fixture cases: an empty compiled RDEF did not
describe the live SRV, and resources with the clamp flag lacked an observed
initial MinLOD. The fixture now compiles live bindings, uses canonical declaration
operands, and explicitly captures initial MinLOD. The second smoke and follow-up
depth probe each retained one CPU/native 3D UAV mismatch. The later complete
matrix's 83 failures led to the expanded private query path, with all original
case selections and exact comparisons retained. Early compiler warnings about
an unsigned mask conversion and a shadowed variable were corrected.

The initial complete resource-query suite passes 623 cases with zero failures or skips in
124.66 seconds: 612 hardware combinations, six independent HLSL comparisons,
three validity/operand/recovery checks and test setup/cleanup. The evidence
summarizer requires the complete unique matrix, all independent comparisons and
all named checks; an intentionally incomplete smoke log is rejected. Original
failed logs remain separate inputs rather than being replaced by the final run.
The four-suite serial regression totals 2,186 passes, zero failures/skips and
542.96 seconds (query 623, texture load 1,445, texture input 76, input snapshot 42).

## Captured extended queries

The first post-implementation Helldivers event 893 run still rejected its first
instruction. Exporting shader 891 showed `resinfo_indexable` with a dimension and
return-type extension (tokens `0x8900103d 0x800000c2 0x00111103`). The executor now
validates each extension against the resource declaration, rejecting duplicates,
unsupported extensions and mismatched dimensions/return types. Six added native
SRV/UAV cases cover all three adapters and deliberately corrupted extension words.
This follow-up changes only query validation; the resource-query suite is rerun
after the earlier four-suite regression.
The final follow-up passes 629 cases with zero failures/skips in 133.67 seconds.
`docs/research/dx11-dxbc-resource-info-evidence.json` records both completed runs,
the unchanged regression results, earlier failures, source/binary identities and
real execution reports. Its complete-release acceptance flag remains false.

The updated Helldivers probe completes 256 group steps and reaches the next
unsupported operation, typed texture UAV storage. Its last completed step is
lane 63, instruction 8, word offset 37. BF1 event 209 retains its previous progress:
11,264 steps and 512 GPU operations before the same texture UAV limitation, with
last completed lane 255, instruction 52, word offset 347. BF1 execution took
13,329 ms (17,398 ms including observation); Helldivers took 1,944 ms including
observation. Neither report claims completed execution or native output parity.
The earlier zero-step rejection remains in the evidence ledger.

## Reproduction and open work

Build `FloraDxbcResourceInfoTests` and run `dxbc_resource_info` serially with the
other selected native DXBC input/load regressions. Completed Qt output is written
to `build/vs2022/dxbc-resource-info-Release.txt`.
`tools/summarize_dxbc_resource_info.py` takes chronological `--log` files,
`--regression` logs, `--source` identities, optional schema-3 `--execution` reports
and `--out`. It requires the complete hardware matrix and preserves earlier
failures separately. Raw logs, native executables and original captures stay
outside Git.

Typed texture UAV reads/writes and their reversible shared storage, sampler
operations, MSAA pixel access, planar queries and complete captured invocation
selection remain open. Query format coverage beyond R32_UINT and native VS/PS
stage comparisons need separate evidence. Full shader parity, GUI cutover and standalone RenderDoc
removal still require their own acceptance evidence.

## Later planar query validation

The [planar SRV increment](research/DX11_NATIVE_PLANAR_SRV.md) adds validated
Y/UV dimensions for NV12, P010 and P016 SRVs, including UNORM/UINT views and
direct original VS/PS/CS comparisons. Its GPU query declaration uses the actual
view return class. The historical R32_UINT evidence above is unchanged; planar
UAV queries and complete shader-debugging parity still need separate validation.
