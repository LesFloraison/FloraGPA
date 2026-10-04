# Native resource discard and capture boundaries

Reviewed 2026-10-04, following revision `6a04b55`. This M2 batch implements the
observed Context4 `DiscardResource`, `DiscardView` and `DiscardView1` layouts.
The [baseline](discard-baseline.json) pins the accepted source, package, original
files, raw-storage checks, original-player comparison and related regression.
The [corpus](discard-corpus.json) keeps explicit negatives alongside positives.

## Semantics and implementation

Discard tells the GPU that selected old contents are unnecessary. It does not
clear to a specified value, release the object, or define replacement storage.
A driver can preserve the memory; byte equality alone cannot prove physical
invalidation. Subsequent reads require defining writes for portable results.
See Microsoft's [resource discard specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#ResourceDiscard)
and [DiscardView1 API](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-discardview1).

The shared checked decoder consumes exact lengths and optional flags, bounds
rectangle storage, and exposes resource/view references in the API log.
Validation requires an immediate context, unlinked record, saved target and
underlying resource, DEFAULT/DYNAMIC usage, matching resource/view dimensions
and non-inverted rectangles. Staging/immutable dropped-call semantics are not
silently reproduced as valid discards. Unknown/missing view layouts reject.

Native replay calls `ID3D11DeviceContext1` at the recorded event, with the saved
view/resource and rectangle values. It does not clear, allocate substitute
objects, regenerate contents, reset counters, or restore pixels from a preview.
Unsupported payload experiments reject. Disabled accepted events are counted
separately by omission from `resource_discards`; unsupported events cannot be
made valid by disabling them. Each performed API call appears in the CLI report
with its event, target, type, count and rectangles. That history records calls,
not physical driver invalidations or a complete final definedness map.

Preflight warns `discard_contents_undefined` for supported records. It certifies
the decoded structure and references, not writes or pixel correctness after
the discard. Undefined-region propagation through shaders, copies and analyzer
histories remains incomplete; no guaranteed original bytes are claimed between
discard and defining writes. The Qt layout is unchanged; existing compatibility
diagnostics and the API inspector expose the new records.

## Original workloads and byte oracles

The optional `FloraDiscardProbe` runs twelve native and twelve injected frames
per mode. The completed production-evidence set has **17 original files / 408
producer frames**. Every frame verifies raw resource bytes against independently
constructed CPU data, and verifies the final RGBA image. SRV mode additionally
draws and reads back the seeded texture before discard to ensure real view use.

| Mode | Original operation | Defining writes / checked scope |
|---|---|---|
| 0 | No discard | Ordinary 2D upload/copy control |
| 1 | DiscardResource, 2D texture | Full subsequent upload |
| 2 | DiscardResource, buffer | All 64 bytes rewritten |
| 3 | DiscardView, RTV | Full subsequent upload |
| 4 | DiscardView, SRV | Verified pre-discard sample, then full upload |
| 5 | DiscardView, UAV | Full subsequent upload |
| 6 | DiscardView, D32_FLOAT DSV | Depth clear from 0.75 to 0.25; raw float bytes |
| 7 | DiscardView1, one RTV rectangle | Rewrite rectangle; verify outside bytes |
| 8 | DiscardView1, overlapping rectangles | Rewrite union; verify outside bytes |
| 9 | Null rectangles, zero count | Full upload; pointer presence is ambiguous in file |
| 10 | Null rectangles, count two | Whole view; full upload; no rectangle bytes serialized |
| 11 | Explicit empty rectangle | No subsequent write; original defined bytes remain |
| 12 | DiscardView, mip 1 / layer 1 RTV | Rewrite selected subresource; verify every mip/layer |
| 13 | DiscardView1, typed R32_UINT buffer UAV | Rewrite elements 4–11; verify all 64 bytes |
| 14 | DiscardResource, 1D texture | Full upload |
| 15 | DiscardResource, 3D texture | Full volume upload |
| 16 | Non-null rectangle pointer, count zero | No subsequent write; pointer presence is ambiguous |

Fifteen files replay; modes 9 and 16 remain explicit scope-information rejections.
Whole-resource/view forwarding uses existing native resource creation; detailed
GPU descriptor validity remains a runtime check. Acceptance above does not
certify every format, dynamic/MSAA/cube/3D-view combination, predication case or
hidden counter dependency. Rectangle execution is currently gated to the
originally tested ordinary 2D RTV and unflagged typed R32_UINT buffer UAV forms.

Hardware/WARP checks verify actual full storage and final pixels twice, with
separate disabled-discard controls. The disabled operation can legitimately
leave the same defined final output; this is an execution-count control, not
proof that discard changed memory. Package exports additionally compare every
accepted resource at a command boundary after its defining writes and before
Present. Output-only green markers for buffer/depth/array/1D/3D modes cannot
substitute for these raw-byte checks.

## Two capture-information boundaries

**Missing view descriptors.** The first originals use RTV/SRV/UAV objects only
in discard calls. GPA records a nonzero target ID but omits its resource entry.
Those files cannot instantiate the original native view. `discard_target_missing`
identifies the event and absent ID; no descriptor is guessed from another view.
Nine such original files remain enrolled as separate negatives.

The later producer actually uses the views: RTV/UAV clears before seeding, and
a sampled draw for SRV. GPA then saves their descriptors. These are new original
captures, not patched versions of the missing-view files. The original datasets,
frozen source/executables and first 18 failed CTest rows are preserved under
`artifacts/m2-discard-originals*` and `artifacts/discards-first-Release.txt`.
Potential safe no-op/overwrite proofs for absent views are not implemented here.

**Zero count loses pointer presence.** The wrapper writes a rectangle-present
byte only when both the pointer and count are nonzero. Real modes 9 and 16 have
byte-identical discard records despite different pointer arguments. Null
rectangles imply whole-view discard; a non-null empty array cannot simply be
replaced by that scope. `discard_rectangle_presence_unresolved` keeps this
distinction explicit. A future safe-use/overwrite proof may admit particular
files; this batch does not infer the pointer from the sample name or later
expected output. Null pointer with a nonzero count is unambiguous in this wire
layout and is passed through as observed.

## Reverse-engineering evidence

Actual injected Context1 vtable slots resolve into the pinned GPA 2025 R1
`shimd3d64.dll`, SHA-256
`cb0b99d113511cbf7ca9f949d97aa02e4a1f8f110b1f3ad3165d8a81035dee31`.
Read-only Ghidra output is `artifacts/discard-wrappers.c`.

| Hook | Vtable slot / wrapper RVA | Wire |
|---|---|---|
| Context4 DiscardResource | 117 / `0x13f210` | `0x3553`: link, context, resource IDs; 24 bytes |
| Context4 DiscardView | 118 / `0x13f900` | `0x3554`: link, context, view IDs; 24 bytes |
| Context4 DiscardView1 | 133 / `0x149d80` | `0x3563`: same 24-byte header, uint32 count, byte presence, optional 16-byte signed rectangles |

Native calls are made before the shim records the observed IDs. Registry lookup
does not itself guarantee that the capture contains the referenced descriptor.
The guarded original primary-chain selector is development-only, as described
in [SO capture delivery](SO_LIFETIME_DISCOVERY.md). GPA DLLs and Python remain
outside the deployed runtime.

## Validation boundary

The baseline records checked truncation of every recovered form, trailing data,
illegal counts/presence bytes, invalid links/context/target/underlying-resource
references, malformed view/resource sizes, usage and dimension mismatches,
inverted rectangles, both real pointer-collision files and the nine original
missing-view files. Original-player final images are separate comparisons;
their adapter/configuration is unidentified and they do not prove intermediate
storage or whether the original player/driver performs a physical discard.
No global image tolerance or shader modification is introduced. M2–M6 remain
incomplete, and the historical full matrix is not relabeled as a new full run.
