# Batched native pixel candidates

`captureNativePixelCandidates` extracts pre-depth coverage from a range of captured
final geometry in one private raster pass. This is a P5 foundation for locating
the primitives whose input quads need further examination. It does not implement
complete pixel history or qualify an original pixel-shader invocation.

## Identity and execution

The geometry bridge now supports expanded point, line and triangle ranges. A
private no-interpolation `uint2` attribute transports the 64-bit source storage
ordinal. It is never substituted for `SV_PrimitiveID`. Existing clip/cull distance,
position, render-target layer and viewport outputs remain present. The existing
single-primitive bridge retains its output format and behavior.

A private device on the exact adapter rasterizes the batch with the original
target dimensions/sample pattern, rasterizer, scissor, sample mask and viewports.
The private observation shader records the selected pixel/sample/layer and quad
coverage. It does not execute the original pixel shader, depth/stencil, predicate,
blend operations or original resource accesses. It requires no free application
UAV slot. The selection's sample is a raster-coverage sample, distinct from the
output-storage sample of the native occlusion API under forced sampling.

Results own the source geometry SHA-256 and metadata, raw observation provenance,
selected lane and source primitive ordinal. Instance and original primitive ID
remain null because no verified mapping is established here. Observation order
remains unspecified; atomic allocation order is not a fragment/blending order.
Repeated observations remain separate, with a sorted unique ordinal index for
lookup. The private PS trace-input join rejects range provenance even if the
range produced exactly one record.

Range/storage arithmetic is checked before allocation. Temporary tagged geometry
and the maximum bridge size share the caller's allocation budget with private
raster resources. The source geometry is caller-owned and excluded from that
additional-allocation budget. Record overflow, invalid ranges, out-of-stride
source attributes and private-semantic collisions fail explicitly. Cancellation
is checked during tagging and GPU completion; failure does not reuse a private
device. These logical allocation checks are not a process peak-memory guarantee.

## Validation

The final serial run passes source boundaries and three Qt suites in 89.53 s:
125 candidate checks, 125 existing pixel-input checks and 24 trace checks, with
no failures or skips. GUI, CLI, Worker and both test/probe targets build in Release.
The first build exposed a namespace typo for `sha256`; its failed log is retained.

The candidate matrix covers 120 cases across NVIDIA, Intel and WARP; one/four
samples; both array layers; every sample; overlap; clipped, offscreen and degenerate
triangles; restricted primitive ranges; scissor and sample-mask rejection. It
contains 660 independent reference draws and 75 candidate observations. The
reference path uses separately compiled VS/PS shaders and ordinary D3D11 draws
with occlusion queries, without the bridge, tagged vertex data, derivatives or
UAV record machinery. Analytical expected coverage is also checked. The result
validator rejects 960 deliberate identity/scope/selection corruptions.

An owned GPA capture with twelve draw scenarios passes 72 measurements across
the same three adapters and two selected pixels. Raw record bytes are decoded
independently to check selected pixel, quad role, coverage bit and ordinal. Source
geometry and record hashes match. Depth/stencil/discard rejection still yields
pre-depth candidates; cull/clip/scissor/sample-mask rejection does not. Both
overlapping triangles are retained. These checks do not invoke original GPA or
RenderDoc; the existing fixture was originally produced by the separate research
workflow recorded in `DX11_PIXEL_HISTORY_DIAGNOSTICS.md`.

Nine real capture probes complete on the three adapters:

| Capture and event | Final primitives | Candidate ordinals | Duration range |
| --- | ---: | --- | --- |
| GF2 113 | 2 | 0 | 207–443 ms |
| BF1 2010 | 5,320 | 0, 162, 441, 578, 1466, 1882, 2044, 2460, 3935, 4961 | 1,160–1,388 ms |
| Helldivers 200 | 4,656 | 0 | 1,138–1,258 ms |

Each capture's three adapters return the same ordinal set. This agreement is a
smoke result, not independent proof of original fragment eligibility. The BF1
set contains ten candidates whereas the earlier native occlusion measurement
returned two passing samples. The difference must be resolved by subsequent
qualification/output extraction, not by dropping candidates or inventing output
values. Durations include replay, geometry extraction, allocation and hashing;
they are not GPU draw timings or release performance acceptance.

## Reproduction

Build `FloraNativePixelCandidatesTests`, `FloraNativePixelCandidatesProbe`,
`FloraDxbcPixelInputsTests` and `FloraDxbcPixelTraceTests`. Run GPU tests serially.

```powershell
$env:FLORA_PIXEL_CANDIDATE_EVIDENCE = '<new-matrix-directory>'
ctest --test-dir build/vs2022 -C Release -R '^(capture_boundaries|native_pixel_candidates|dxbc_pixel_inputs|dxbc_pixel_trace)$' -j 1 --output-on-failure
python tools/validate_native_pixel_candidates.py <new-matrix-directory> --out <new-report.json>
python tools/run_native_pixel_candidate_fixture.py <existing-owned-fixture-directory> --probe build/vs2022/Release/FloraNativePixelCandidatesProbe.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out <new-fixture-report-directory>
build/vs2022/Release/FloraNativePixelCandidatesProbe.exe <frame> <event> <x> <y> <sample> <layer> <vendor-or-zero-for-WARP> <new-output-directory>
```

The fixture runner imposes a 150-second subprocess watchdog. The probe also
checks a 120-second cooperative deadline; driver calls still require the external
watchdog. Raw captures, geometry, records, binaries and logs remain ignored. The
compact ledger is `dx11-native-pixel-candidates-evidence.json`.

## Remaining gates

Preserving captured geometry alone does not recover stateful original shader
execution, original fragment ordering, depth/stencil/blend transitions, failures
or shader outputs. Geometry with no selected range and batches exceeding the
record/budget limit need orchestration in the future history job. Public history
and PS debugging are not switched by this internal API. Original instance and
primitive-ID mapping, original execution qualification, full P5/P6 parity and
P7/P8 dependency-removal acceptance remain required.
