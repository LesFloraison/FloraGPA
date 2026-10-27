# Private native pixel output merger

`mergeNativePixelOutput` consumes the project's compact native pixel-shader
outputs and applies one lane to a private single-sample color/depth storage cell.
This advances the P5 path from shader outputs to depth/stencil and blend effects.
It is not a replacement for complete fragment history or original eligibility.

## Execution and result contract

The operation creates a separate D3D11 device at the reference feature level and
verifies its adapter LUID. The input shader-output adapter must match before any
private draw. It never obtains or mutates the reference immediate context.
Known raw color bits enter a typed constant-output pixel shader. The hardware
output merger applies the requested blend factors, independent target slot,
write mask, logical operation, sample mask, depth/stencil state, face selection,
stencil reference and read-only DSV flags. Initial storage is uploaded as raw
bytes; staging readback preserves resulting storage bytes.

Dual-source factors include the second shader output without binding a second
target. Alpha-to-coverage includes target-zero alpha. `SV_Coverage` is declared
only when present in the supplied shader outputs, preserving its interaction
with alpha-to-coverage. Discarded or inactive lanes execute a private discard.
Depth uses the shader's known depth output, or the caller's raster-depth bits.
When depth storage is bound, one of those observations must be present; an absent
raster-depth observation remains null and cannot silently become depth zero.
The private shader emits ordinary `SV_Depth`; this deliberately models the
post-shader operation, not original early-depth behavior or conservative-depth
optimization. The synthetic triangle selects front/back stencil state without
reconstructing original rasterization.

The versioned result includes before/after color and depth bytes, exact source
quad-result hash, adapter, selected lane/target, descriptor words, factors and
masks. The occlusion value retains D3D11 query semantics. It is not promoted to
original fragment identity or inferred from whether storage changed. Original
fragment eligibility stays null and early-depth reconstruction stays false.

The API currently requires complete known color outputs for every output consumed
by the selected target, dual-source blending or alpha-to-coverage. Missing values
fail explicitly; they are not replaced with guessed colors. It supports one
single-sample cell and an optional depth cell, not MSAA reconstruction. These are
limits of this internal building block. Public history/debug routing has not
changed, so they do not narrow existing user-facing support.

Cancellation is checked before setup/submission and during bounded completion.
Failures discard all private resources. Repeated calls start from the supplied
original storage. Unsupported native format/state combinations fail on the
selected adapter; no adapter or external-backend fallback is attempted.

## Validation design

The tests build owned GPA fixtures using existing repository capture builders.
An independent before-event replay supplies resource snapshots; the C++ DXBC
executor produces the compact shader outputs; the new merger consumes them.
A separate full replay executes the fixture's original VS/PS draw with the same
experiment descriptors. Exact raw color/depth readback is compared, and the
before-event reference storage is checked again after the private merger.
The full replay does not execute the merger or its generated shaders.

The matrix covers NVIDIA, Intel and WARP; all supported D3D11 RGB blend factors on
both source and destination paths; all five blend operations; dual-source
blending; all sixteen logical operations; independent slots 3/7; color/sample
masks; discard; coverage output; alpha-to-coverage; and float, normalized, packed,
BGRA and sRGB storage. Depth tests cover both faces, all comparison functions,
all eight stencil operations on fail/depth-fail/pass paths, read/write masks,
read-only depth/stencil flags, D16/D24S8/D32 storage, and ordinary/conservative
shader depth outputs. Boundary checks include
wrong adapter/lane, unknown color components, inconsistent lane state, malformed
storage, flags, missing depth observations, cancellation, timeout bounds and repeat isolation.

`validate_native_pixel_merger.py` independently checks matrix completeness,
source hashes, result scope and raw GPU comparisons. It also computes depth,
stencil and logical-operation expectations on the CPU. Its negative controls
must reject changed scope, invented eligibility, early-depth claims, broken
isolation, altered source identity and missing output storage.

The final matrix has 546 GPU cases and 3,276 rejected corruptions. The final
merger Qt suite passes 549 checks in 75.13 s through CTest. The unchanged native
output, depth/stencil and blend regression suites pass 95, 4 and 7 checks,
respectively; capture boundaries also pass. Together these comprise 655 Qt checks
and five distinct CTests, with no failures or skips. GUI, CLI, Worker and both
native test targets build in Release. No existing goldens, assertions or skips
were changed. Development runs and the final 572-artifact hash manifest are
retained separately; timings are test-suite observations, not release latency
or peak-memory acceptance.

No original GPA binary or RenderDoc is needed by this validation. The tests use
the project's replay of owned fixtures, not Intel GPA execution. Real game
capture integration and multisample/per-fragment ordering remain separate gates.
Final counts, timings, binary/source identities and raw artifact hashes are
recorded in `dx11-native-pixel-merger-evidence.json`.
The test executable's static imports contain no RenderDoc or private GPA DLL;
this is not a dynamic module audit. GPA debugger-engine closure, full P5/P6
parity, dependency removal and standalone P8 release acceptance remain open.

## Reproduction

Run from the repository root with Qt on PATH and a new evidence directory:

```powershell
cmake --build build/vs2022 --config Release --target FloraNativePixelMergerTests FloraNativePixelOutputsTests FloraGPA FloraGPA.Cli FloraGPA.Worker --parallel 4
$env:FLORA_PIXEL_MERGER_EVIDENCE = 'D:/results/pixel-merger/matrix'
ctest --test-dir build/vs2022 -C Release -R '^(capture_boundaries|native_pixel_merger|native_pixel_outputs|depth_stencil|blend)$' --output-on-failure -j 1
python tools/validate_native_pixel_merger.py D:/results/pixel-merger/matrix --out D:/results/pixel-merger/validation.json
```

The SDK contracts used here are documented in Microsoft's
[blend descriptor](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/ns-d3d11_1-d3d11_render_target_blend_desc1)
and [depth/stencil descriptor](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_depth_stencil_desc).
Local native draws and independent CPU checks establish the recorded behavior;
those contracts alone are not acceptance evidence.
