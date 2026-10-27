# Pixel merger target coordinates

## Defect and implementation

Implementation commit: `5bb512ff7eece5169f0aae8b27ad871593f26e8c`.

The private output merger previously moved every selected texel to `(0, 0)` in
a 1x1 target. That loses the original raster position. The owned WARP fixture
demonstrates an observable error: with 2x MSAA and alpha 0.125, the original draw
writes sample 0 at `(0, 0)` and `(6, 4)`, but writes neither sample at `(1, 1)`
and `(4, 2)`. At alpha 0.625 its masks are respectively 3, 1, 1 and 3. These
are local observations, not portable alpha-to-coverage rules for other drivers.

`native-pixel-merger/3` accepts the selected `x`, `y`, target `width` and `height`.
It creates private resources of that extent, initializes the selected texel in
each sample, and rasterizes at the requested pixel location. Other private
texels are zeroed. Reads honor the mapped row pitch and return only the selected
texel per sample. The reference device's adapter identity remains mandatory;
there is no implicit adapter switch. Defaults retain the previous 1x1 behavior.

The result records `private_pixel_location`, `private_target_extent`,
`logical_payload_bound` and `payload_limit`. Invalid extents, out-of-range
coordinates and zero/exceeded budgets are rejected. The default logical payload
limit is 256 MiB; the conservative calculation sums
`width * height * texel_bytes * (2 * sample_count + 4)` over color and depth.
This bounds the accounted texture/transfer/CPU payload, not driver allocation
overhead or process peak memory. Full-size targets can therefore fail the
explicit budget; silently relocating the pixel is not a fallback.

D32S8's X24 bytes are marked invalid for both single-sample and multisample
results; returned padding is canonically zero. Padding is not recovered data.
The original shader outputs, raster depth and coverage still come from the
caller. This helper does not reconstruct original fragment eligibility, early
depth, sample-frequency invocation identity, primitive order or UAV side effects.
Public history and pixel debugging are not switched by this change.

## Validation

All GPU cases run serially on the local WARP, NVIDIA and Intel adapters with
the D3D11 debug layer enabled. Every comparison uses a separate replay of the
original captured shader and original full-size viewport, rather than the
merger's generated shader as its oracle. The entire reference color/depth
storage is compared before and after the private diagnostic.

The existing 546 single-sample and 144 multisample cases retain their assertions.
The new alpha-to-coverage matrix has 144 cases: three adapters, 1x/2x/4x samples,
four locations in a 7x5 target, and four alpha values. A further 72 cases cover
D16, D24S8, D32 and D32S8 at `(1, 1)` and the lower-right `(6, 4)`, using the same
adapters and sample counts. Alternating sample masks verify untouched samples.
Depth/stencil results also have independent numeric and stencil expectations.

The first focused run failed 48 single-sample fixture loads because the synthetic
data header contained a row byte count instead of the full byte count. Its 96
MSAA cases passed. The fixture encoding was corrected; no oracle, assertion or
skip condition was weakened. These failure logs remain in the evidence. A
subsequent 837-check complete run and the 74-check depth extension passed. Review
of the extension then exposed the missing single-sample D32S8 invalid-byte marker,
which was corrected before the final combined run.

The final combined CTest run passes in 175.26 seconds: 909 Qt checks, zero
failures and zero skips. Its four validation matrices contain 906 GPU cases and
reject 9,738 corrupted reports. The remaining checks are initialization, cleanup
and boundary/cancellation/retry assertions. This is suite elapsed time, not an
individual analysis latency benchmark.

A single in-flight process snapshot contains no RenderDoc or original GPA
installation modules. Its observed peak working set is 134,647,808 bytes at the
snapshot time; it is not a final process peak or a transient-load audit.

The validator checks matrix completeness, native source digest, adapter identity,
storage lengths, requested coordinates/extent, payload accounting, raw original
draw results, deterministic depth/stencil behavior and explicit unknown fields.
It continues to accept historical version-2 evidence only with its original
1x1 location. New coordinate reports require version 3.

## Reproduction

Use a fresh evidence/output path for each run; existing evidence is never
overwritten. Build `FloraNativePixelMergerTests` in Release first, then run:

```powershell
$env:FLORA_PIXEL_MERGER_EVIDENCE = "$PWD/artifacts/dx11-native/pixel-merger-coordinates-rerun"
ctest --test-dir build/vs2022 -C Release -R '^native_pixel_merger$' --output-on-failure
python tools/validate_native_pixel_merger.py "$env:FLORA_PIXEL_MERGER_EVIDENCE" --out single.json
python tools/validate_native_pixel_merger.py "$env:FLORA_PIXEL_MERGER_EVIDENCE/msaa" --msaa --out msaa.json
python tools/validate_native_pixel_merger.py "$env:FLORA_PIXEL_MERGER_EVIDENCE/coordinates" --coordinates --out coordinates.json
python tools/validate_native_pixel_merger.py "$env:FLORA_PIXEL_MERGER_EVIDENCE/depthCoordinates" --depth-coordinates --out depth.json
```

The adjacent evidence ledger pins source, executable, raw reports, validation
reports and development/final logs. Raw artifacts remain outside Git. This
synthetic check does not establish real-capture fragment parity, absolute peak
memory bounds, transient module audit coverage, or standalone package acceptance.
P5/P6 integration and P7/P8 dependency removal remain open.
