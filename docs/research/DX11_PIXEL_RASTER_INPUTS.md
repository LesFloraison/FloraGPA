# Private selected-pixel input observation

This checkpoint connects captured final geometry to a selected pixel's native
input observation. It does not complete original-fragment selection, Pixel
History or the native pixel-debugger job/UI. RenderDoc removal acceptance remains
open. The implementation is independent C++/DXBC/D3D11 code and does not load GPA
private binaries or copy another debugger's interpreter.

## Execution and state

`DxbcPixelRaster` snapshots the native target view, rasterizer, viewports, scissors
and sample mask. It resolves mip dimensions and view-relative array/depth-slice
extents, including the all-remaining-slices sentinel. It creates a private device
on the exact reference adapter LUID and feature level. WARP is explicit; an
adapter mismatch fails instead of selecting another GPU. Debug-layer validation
is inherited when the reference device enables it.

The private device runs only the generated geometry bridge and observation PS.
No application shader resources, UAVs, depth/stencil contents, predicate or
hidden counters are used or changed. A single carrier view retains the original
typed format, dimensions, layers, sample count and quality. Its color writes and
depth/stencil tests are disabled. A missing target remains absent. Logical
resource sizes are checked against a caller-supplied budget before allocation;
the report does not present this estimate as measured driver/peak memory.

Original viewport and sample coordinates are unchanged. Private scissors
intersect the original scissors with `[x-1, y-1, x+2, y+2]`, retaining every possible
2x2 neighbor of the chosen pixel. Missing original scissors remain empty when
scissoring is enabled. This bounds raster work for a large source primitive;
full-size carrier allocation remains necessary in this implementation.

The selected sample denotes the rasterization sample. `ForcedSampleCount`, when
present, determines its range independently of target storage samples. Layer
selection is relative to the original bound view. A geometry layer output
requires an actual observed `SV_RenderTargetArrayIndex` input. A missing layer
output has D3D11's default layer zero, rather than an inferred captured value.

## Raw record transport

The original input-declaration transport now has an optional record variant.
It owns private raw UAV `u1` because the original PS is replaced entirely. It
does not require a free UAV slot in the application's PS bindings.

The first uint32 is an atomic record count; bytes 4 through 15 are reserved zero.
Records start at byte 16 and contain nine uint4 values (144 bytes):

| Index | Contents |
| --- | --- |
| 0–3 | Raw input bits for the four measured quad lanes |
| 4 | Pixel x/y and measured horizontal/vertical lane role |
| 5 | Four observed sample indices |
| 6 | Four observed coverage masks |
| 7 | Raw floating-point orientation diagnostics |
| 8 | View-relative layer, selected raster sample, zero, zero |

Capacity is bounded to 1–4096 records. A count beyond capacity fails; a partial
prefix is never accepted as a complete result. Empty results and multiple
records remain explicit, with unspecified order. Undeclared input components
remain invalid even though their transport padding is zero. Neighbor values
do not by themselves establish original helper-lane validity or execution order.

The first implementation let the HLSL compiler sink derivatives into the
selected-pixel branch. Its generated-DXBC uniformity check rejected that shader.
The corrected shader reconverges after conditional counter allocation and runs
all transport derivatives/stores unconditionally. Nonselected and overflow
invocations use an address beyond the raw UAV view. D3D11 defines those UAV
writes as discarded, including component-wise bounds behavior; see Microsoft's
[store_raw instruction specification](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/store-raw--sm5---asm-).
The shader generator still rejects conditional derivative instructions. The
initial failure log is retained without weakening the check or any comparison.

Cancellation is checked before work, around submission and during bounded GPU
readback. Existing `GpuCompletion` supplies the five-second default readback
deadline. Driver object creation itself is synchronous; this helper is not a
replacement for the Worker's process-level timeout/recovery boundary.

## Validation

The input suite passes 125 checks with no failures or skips. Its 102 GPU fixtures
span NVIDIA, Intel and WARP, 1x/4x samples, interpolation modes, integer bits,
sparse masks, minimum precision, system values and SM4/SM4.1 originals. Each
private selected-pixel record matches all eight existing MRT observations
exactly: 3,264 uint32 components. The unchanged independent GPU oracle still
checks 129,600 direct-input and 518,400 quad-input components.

All 32 selected registers from both ordinary and packed full signatures create
record shaders on all three adapters. Additional contracts cover mip/array and
3D view extents, explicit layer forwarding, zero sample mask, excluded/missing
scissors, wrong-layer empty observations, budget rejection, cancellation before
work and after submission, deterministic retry and preserved original RTV
binding. The original quad and sampling regression suites pass another 1,022 and
168 checks respectively, with no failures or skips. GUI, CLI and Worker Release
targets also build; their existing RenderDoc dependencies are not removed by
this checkpoint.

GF2 event 113, BF1 event 2010 and Helldivers event 200 each produce nonempty
selected-pixel records on all three adapters, covering 33 declared input
registers. Primitive ordinal zero is selected. The probe can choose a projected
primitive-centroid candidate; it saves that selection and distinguishes an
empty observation from a failure. These real probes validate acquisition and
provenance, not independent original-PS input equivalence. The retained geometry
roundtrips compare another 675 raw components exactly.

The offline record verifier checks layout, reserved/unallocated bytes, adapter,
raster state, selection, coverage, sample identity, geometry source mappings
and exact synthetic MRT values. Seven independent corruptions of count, header,
pixel, orientation, layer, unused storage and input bits must be rejected. The
existing input and geometry validators retain their twelve corruption controls.

## Reproduce

Build `FloraDxbcPixelInputsTests` and `FloraDxbcPixelGeometryProbe`. Use a fresh
evidence directory and run GPU commands serially:

```powershell
$env:FLORA_PIXEL_INPUT_EVIDENCE = 'new-synthetic-directory'
& build/vs2022/Release/FloraDxbcPixelInputsTests.exe -o new-tests.txt,txt
Remove-Item Env:FLORA_PIXEL_INPUT_EVIDENCE
& build/vs2022/Release/FloraDxbcPixelGeometryProbe.exe frame.gpa_frame 2010 0 4318 new-real-directory centroid
```

The last argument can instead be explicit `x y sample layer`. Vendor 4318 is
NVIDIA, 32902 is Intel and zero explicitly requests WARP. A real run without
declared PS inputs returns 3; an empty raster observation returns 4. A failed
request returns 1. Six-argument geometry-only probe usage remains available.

```powershell
python tools/validate_pixel_input_capture.py new-synthetic-directory --out new-input-validation.json
python tools/validate_pixel_geometry.py new-synthetic-directory real-directories --out new-geometry-validation.json
python tools/validate_pixel_raster.py new-synthetic-directory real-directories --out new-record-validation.json
```

For the record verifier, `real-directories` expands to the nine capture/adapter
directories. Raw observations, captures and binaries stay outside Git. The
adjacent evidence ledger records their hashes and exact accepted results.

## Remaining limits

The scope is isolated-primitive, pre-depth input observation. Original
depth/stencil qualification, discard, alpha-to-coverage, blending, predication,
original side effects and fragment order are not reconstructed here. A separate
register pass cannot silently identify ambiguous records across passes. PS
programs without ordinary input registers still need an invocation acquisition
path. Boundary/helper validity, full point/line and conservative/forced-sample
rasterization behavior require broader probes. Actual GPU record overflow has
not been forced in this matrix; its rejection is checked by corrupted evidence.
Full Pixel History, native pixel debugging integration, GPA debugger-engine
recovery, dependency deletion and standalone package acceptance remain open.
