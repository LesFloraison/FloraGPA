# Native depth-only output merger

## Scope and implementation

The private output merger previously required a complete color output and created
an RTV even when only a depth/stencil target was selected. Backend
`native-pixel-merger/4` permits either target to be absent and requires at least
one bound target. An absent target must not contain storage; absent depth must
not contain DSV flags. The merger creates and binds only the supplied targets.

A depth-only draw requires observed raster depth or an evaluated shader depth
output. It does not require a color output unless alpha-to-coverage consumes
target-zero alpha. Dual-source blend settings do not require a second color
output when there is no color target. Invalid or absent unused color values do
not become invented depth data. Same-adapter device selection, private resource
ownership, bounded payloads, deadlines and cancellation remain in effect.

This is a P5 component increment. The helper consumes an already evaluated
private shader lane; it does not establish original raster coverage, original
early-depth execution, fragment identity, writable shader scheduling or failure
attribution. `original_fragment_eligibility` remains null and
`original_early_depth_reconstructed` remains false. Public full Pixel History and
`debug-pixel` are not enabled by this change.

## Independent probes

The new matrix contains 432 GPU cases: WARP, NVIDIA and Intel; 1x, 2x and 4x MSAA;
D32S8, D32, D24S8 and D16; and applicable combinations of raster depth, shader
depth, discard, sample mask, shader coverage, alpha-to-coverage, disabled depth
writes, read-only depth, failing depth, front/back stencil operations, failing
stencil, read-only stencil and same-value depth writes. Stencil-only cases are
defined only for formats that contain stencil. This is explicit matrix design,
not a runtime skip or an expansion of historical failure allowances.

Each case replays an owned capture with the original shader and actual D3D11
output-merger state, with no RTV bound. The selected texel is the lower-right
pixel `(6, 4)` of a 7x5 target. A separate private merger invocation uses the
native executor's outputs and compares every valid depth/stencil byte to the
original draw. D32S8 padding is explicitly invalid and normalized. The original
resource is read again after diagnosis to check state isolation.
The raster-only fixture binds a void pixel shader; an entirely unbound pixel
shader is not covered by this matrix.

An independent occlusion query measures the entire original draw; it is labeled
separately from the private selected-pixel query. The same-value case uses
`DepthFunc=ALWAYS`, enabled depth writes and shader raster depth 0.5 over a depth
buffer cleared to 0.5. Both queries confirm all samples pass while bytes remain
unchanged. This proves that storage equality alone cannot identify absence of a
write in this controlled probe; the private query does not become original
fragment identity.

The Python validator checks the full matrix, source digests, adapter identity,
state descriptors, validity, sample isolation, queries and deterministic numeric
depth/stencil expectations. Alpha-to-coverage is compared to the original draw
on the same adapter. Its selected-sample result must be one of the independently
checked preserved/written values, and its query must agree. No universal coverage
pattern or uniform full-draw coverage is assumed for that hardware-defined case.

The D3D11 debug layer emits the expected warning for the alpha-to-coverage shader
writing target zero while no RTV is bound. The message explicitly permits this
configuration and discards the unbound color write. Debug error checks remain
enabled. No warning filter or error assertion was weakened.

## Validation results

The final complete suite passed 1,341 Qt checks with zero failures and skips in
296.989 seconds. Its five matrices contain 1,338 GPU cases and reject 17,514
corrupted reports. The existing 906 cases retain their numeric assertions. The
boundary test additionally checks absent targets, illegal storage on an absent
target, missing depth data, missing alpha for alpha-to-coverage, irrelevant
dual-source state and unknown unused color values, alongside existing
cancellation and retry checks.

The historical version-3 corpus also passes all 906 cases with the validator's
default behavior. An initial corpus-path check selected the older 834-case
directory, which lacked the later depth-coordinate extension; the corrected
check used the complete final corpus. Both logs are retained. No golden data or
assertion was modified to accommodate this change.

A single in-flight module snapshot observed no RenderDoc or original GPA
installation modules, with a peak working set of 119,111,680 bytes at that point.
This is neither the final process peak nor a transient-load audit. Suite elapsed
time is not an individual job latency benchmark.

The focused matrix passed 434 Qt checks (432 cases plus lifecycle), with zero
failures or skips, in 123.525 seconds. Its validator rejected 7,776 corrupted
reports. One subsequent build command used an incorrect CLI target spelling;
MSBuild rejected the nonexistent project. The corrected command using
`FloraGPA.Cli` completed successfully. Both command logs are retained; this was
an invocation error, not a compiler or test failure.

## Reproduction

Build `FloraNativePixelMergerTests`, `FloraGPA`, `FloraGPA.Cli` and
`FloraGPA.Worker` in Release. Use fresh paths because evidence files are opened
exclusively, and run GPU tests serially:

```powershell
$env:FLORA_PIXEL_MERGER_EVIDENCE = "$PWD/artifacts/dx11-native/depth-only-rerun"
ctest --test-dir build/vs2022 -C Release -R '^native_pixel_merger$' --output-on-failure
python tools/validate_native_depth_only_merger.py "$env:FLORA_PIXEL_MERGER_EVIDENCE/depthOnly" --out depth-only.json
python tools/validate_native_pixel_merger.py "$env:FLORA_PIXEL_MERGER_EVIDENCE" --backend native-pixel-merger/4 --out single.json
python tools/validate_native_pixel_merger.py "$env:FLORA_PIXEL_MERGER_EVIDENCE/msaa" --backend native-pixel-merger/4 --msaa --out msaa.json
python tools/validate_native_pixel_merger.py "$env:FLORA_PIXEL_MERGER_EVIDENCE/coordinates" --backend native-pixel-merger/4 --coordinates --out coordinates.json
python tools/validate_native_pixel_merger.py "$env:FLORA_PIXEL_MERGER_EVIDENCE/depthCoordinates" --backend native-pixel-merger/4 --depth-coordinates --out depth.json
```

Historical validator behavior remains unchanged without `--backend`. Version 4
requires explicit selection for the four existing matrices; their numerical and
state assertions remain intact. The depth-only validator requires version 4.

Raw reports, logs and executable snapshots remain outside Git. The adjacent
evidence ledger records hashes. This synthetic probe does not close real-capture
fragment parity, original GPA debugger-engine recovery, final transient module
auditing or P7/P8 standalone dependency-removal acceptance.
