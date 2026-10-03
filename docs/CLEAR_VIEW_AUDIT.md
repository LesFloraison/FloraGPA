# M2: ClearView execution and resource boundaries

The normalized immediate-context record `0x257` now executes
`ID3D11DeviceContext1::ClearView` with the captured view, float color and signed
rectangles. This is a resource write, never an auxiliary no-op. GPU predication
remains in effect. The replay checks native ClearView support and, for pure
depth views, `ClearViewAlsoSupportsDepthOnlyFormats`. Failed support checks
produce an explicit error rather than a silently dropped call.

## Wire and original-player evidence

Read-only Ghidra output is retained in `artifacts/m2-clear-view-wrapper.c`
(Context4 wrapper RVA `1494a0`, string RVA `4be0d0`). Its immediate path emits
`0x257`; its linked path emits `0x3562`, which remains unsupported. The payload
contains link/context/view IDs, an optional 16-byte float color, a uint32 rectangle
count, and a rectangle-array flag followed by four signed 32-bit coordinates
per rectangle. The color-present base is 46 bytes. The writer canonicalizes zero-count
rectangles to an absent array even when the application passed a non-null pointer.

The independent original player's resource initialization and execution RVAs
are `1bca0` and `26060` in `artifacts/m2-clear-view-player.c`. Execution resolves
the view, queries Context1 and invokes vtable slot `0x420` with color, rectangle
pointer and count. The wrapper's native forwarding helper is separately retained
in `artifacts/m2-clear-view-writer.c`. Native forwarding and serialized writing
are distinct evidence; the wrapper contains the actual record layout.

The decoder rejects linked records, missing color, invalid flags, truncated or
trailing bytes, and present rectangle arrays with zero or more than 65,536
elements. Null rectangle pointers retain the raw count without allocating an
array. Validation checks immediate context, view/resource references, descriptor
sizes and dimensions, non-inverted rectangles, buffer rectangle height, and
writable depth-only views. Video, 3D, stencil-bearing/read-only depth views and
raw/structured/flagged buffer views are explicitly outside this batch's scope.

These semantics follow the
[Microsoft ClearView contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-clearview).
The native call performs format conversion, slice coverage and clipping; replay
does not reinterpret integral-float colors as uint bit patterns. Empty rectangles
remain no-ops. The UI exposes decoded API fields and the existing command-disable
experiment, without adding a color/rectangle editor.

## Original fixtures and exact oracles

`tools/native/clear_view_probe.cpp` produces sixteen unmodified CaptureNextFrame
files. Each native and shim-injected run independently checks twelve frames,
giving 384 producer frame checks. All resource bytes and final images are compared
against CPU-generated expectations, not output learned from FloraGPA.

| Mode | Coverage |
|---|---|
| 0 | Full backbuffer RTV clear |
| 1 | One rectangle |
| 2 | Overlapping rectangle union |
| 3 | Zero-width/zero-height rectangles |
| 4 | Texture2D-array RTV, mip 1 and selected slices |
| 5 | Texture2D-array UAV, mip 1 and selected slices |
| 6 | D32 depth-only array DSV, mip 1 and selected slices |
| 7 | R32_FLOAT buffer RTV with nonzero first element |
| 8 | R32_UINT buffer UAV with integral-float conversion |
| 9 | Four-sample RTV, checked through its subsequent resolve |
| 10 | Texture1D-array RTV |
| 11 | UNORM conversion of negative infinity, positive infinity and NaN |
| 12 | Rectangles clipped at the surface edges |
| 13 | Non-null rectangle pointer with zero count, canonicalized by the writer |
| 14, 15 | True occlusion predicate, suppressed/permitted ClearView |

Modes 6, 7, 8 and 10 use a constant green presentation marker; their final images
alone say nothing about the cleared resource. Mandatory raw-resource boundaries
and hardware/WARP byte tests establish those results. Mode 9 compares resolved
storage; it does not claim separate sample-by-sample exports. D16 is structurally
accepted and delegated to the native API, but these fixtures independently
certify D32 only. Other formats/combinations need their own acceptance evidence.

All 32 hardware/WARP cases run two unobserved/observed pairs using the same Replay
instance, verify all bytes, and run a separate disabled-command control. The
empty-rectangle and predicated-skip cases deliberately remain unchanged; all
other disabled controls must change resource bytes. Negative copies exercise
bad references, descriptor lengths/dimensions, inverted bounds, malformed flags,
counts and all truncated command prefixes. These copies are not accepted captures.

The [corpus](clear-view-corpus.json) retains capture hashes and exact byte/image
oracles. Before implementation all sixteen captures failed preflight on the
unsupported ClearView record (`artifacts/m2-clear-view-before/validation.json`).
The strengthened corpus runner now enforces resource hashes itself and supports
buffer exports; its separate [acceptance](resource-boundary-baseline.json) also
retains stable-but-wrong hash counterexamples using the previously accepted binary.

## Reproduction and remaining scope

The package `out/FloraGPA-clear-view-20261003` passes the serial 270-file matrix:
540 ordinary independent runs, 269 stable files and one known-variable Helldivers
file. All 253 prior stable hashes are unchanged. All 293 diagnostic control runs,
120 resource boundary exports (116 texture and four buffer), and four golden
checks pass. The sixteen new files supply 32 of those strict byte boundaries.
Their 32 original-player runs have identical final images; the marker-image and
device-configuration qualifications above remain in force. Exact source/binary
hashes and supplemental test records are pinned in
[clear-view-baseline.json](clear-view-baseline.json).
All 32 related CTest suites pass, including 36 ClearView and 55 Qt/Worker cases
without skips. The five packaged application binaries match the Release build.

Build `FloraClearViewProbe` explicitly, then run
`<fresh-output-directory> <mode> [<capture.gpa_frame> <shimloader64.dll>]`.
Capture runs require `GPA_LOCAL_INJECT=true`. Set `FLORA_CLEAR_VIEW_CAPTURES` to
the numbered capture root for `clear_view` CTest. Run GPU checks serially. GPA,
Python, captures and generated evidence remain development-only and outside Git.

This implements normalized immediate ClearView, not linked/deferred execution,
video clearing, all format combinations or general resource version recovery.
CopySubresourceRegion1/UpdateSubresource1 and broader interface semantics remain
M2 work. M3–M6 remain incomplete; module migration totals are unchanged.

Historical evidence note (subsequent Context1 transfer batch): its CTest wrapper
accidentally overwrote this batch's aggregate `artifacts/ctest-m2-clear-view.log`.
The old aggregate is unavailable; the baseline's original hash has not been
rewritten. Archived per-suite Qt results, corpus reports and package/source
evidence remain. See the [transfer audit](TRANSFER1_AUDIT.md) for the incident
record and fresh regression evidence. The transfer batch supersedes the remaining
CopySubresourceRegion1/UpdateSubresource1 work above within its documented scope.
