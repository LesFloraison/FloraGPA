# Live output-merger binding snapshots

## Implementation

`snapshotNativePixelMergerState` reads current D3D11 bindings into the owned
private merger state. An explicit color slot must be bound; an absent color
selection requests depth-only analysis. A bound DSV is included for color
analysis. The snapshot records GPA resource/view identities, actual typed view
descriptors, mip, view-relative and absolute layer or volume slice, extent,
sample count/quality and DSV read-only flags. Blend state (including logic ops),
blend factors, sample mask, depth/stencil state and stencil reference come from
the current context. D3D11 default state is used only when the corresponding
state object is actually unbound.

Storage is selected from live resource metadata, not assumed from a captured
descriptor. The new `Replay::observeMsaaStorage` observes the current resource
description and returns an explicitly selected sample; it never resolves.
Snapshots preflight aggregate readback payload bytes and retain native texels in
sample-major order. Missing targets, out-of-range coordinates/layers, unknown
GPA identities and incompatible target extents/samples fail explicitly.

The snapshot reports whether each transfer preserves exact storage bits.
Integer-family and depth/stencil MSAA transfers provide that guarantee. The
existing converted BGRA MSAA path remains usable and is explicitly marked
`exact_storage_bits=false`. It must not be promoted to exact original storage
by a later history join. Content definedness and original fragment eligibility
remain unknown; D32S8 padding bytes remain invalid and are normalized separately.

Raster depth, face and geometric coverage do not come from OM bindings. The
snapshot records `raster_inputs_supplied=false`; callers must join separately
observed invocation data before using these fields. This change does not yet
integrate the complete original-fragment history pipeline or expose public
pixel debugging. Its state object is internal and is not a standalone cached
analysis result.

## Cancellation and state preservation

Texture-storage reads supplied with a cancellation callback use
`D3D11_MAP_FLAG_DO_NOT_WAIT`, flush queued work and poll with cancellation and
device-removal checks. Calls without cancellation preserve their previous
blocking behavior. Live MSAA observation supplies a bounded callback to both
its transfer setup and storage readback. The shared transfer checks between
resource/view construction, shader compilation, array passes, command submission
and readback. The Worker process remains the outer recovery boundary for a
driver call that itself does not return.

Existing unpredicated inspection, predicate segmentation, deferred command-list
state restoration and WARP copy-binding isolation remain in use. The snapshot
does not advance the captured draw. Selection tests cancel after setup begins,
retry on the same replay and compare the same storage. The merger matrices also
check original storage again after diagnosis and compare output to a separate
normal GPU replay.

## Probes and retained failure

The existing merger matrices now obtain actual blend/depth descriptors and
storage from the snapshot before executing the private merger. Fixtures retain
their separately known raster depth/face. Where a fixture represents geometric
coverage through the application sample mask, the test first verifies the
observed combined mask, then restores that known factorization. Expected normal
GPU outputs and all previous numerical assertions remain unchanged.

Twelve new selection cases cover three adapters, 1D/2D arrays, 3D slices and MSAA
arrays. They select RTV slot 2 through leading unbound slots, nonzero mip/layer
and lower-right coordinates. Patterned integer texels provide independent exact
expectations. Tests also exercise an exact readback budget, a budget one byte too
small, invalid layers, unbound color/depth selections, cancellation and retry.

The first complete run passed 1,347 checks and failed six BGRA MSAA cases because
the new snapshot required a bit-exact transfer unconditionally. That incorrectly
excluded an existing converted path. The correction preserves its actual
precision metadata instead of inventing exactness or deleting support. The
initial failure log and all produced reports are retained. No golden data or
assertion was weakened.

The first offline depth-only validation also found 198 descriptor differences.
The old fixture supplied authored stencil operations even when stencil was
disabled; D3D11 creation returned canonical disabled-stencil operations. The
live snapshot correctly returned those actual fields. The updated probe creates
an independent depth/stencil state on the same device and queries its descriptor
without using the snapshot or context binding. Reports retain authored,
independently queried and snapshot descriptors separately. Validation requires
exact authored fixture fields, exact agreement with the independent query and
exact active fields. All numerical and query assertions remain in place, and
historical reports without live snapshots keep their original strict checks.
The initial mismatch list remains evidence, not a passing result. The historical
432-case depth-only corpus still passes the unmodified historical branch of the
validator, including 7,776 rejected corruptions.

The first run with the independent descriptor probe stopped without a Qt totals
line. On continuation its process handle was missing and no test process was
running. Its partial reports remain separate; a fresh full run uses the
`pixel-merger-snapshot-complete` directory. Partial reports are not counted as a
completed matrix.

The readback limit bounds aggregate logical storage payload, not total driver
allocations or peak working set. The helper is internal: complete frame,
experiment and adapter cache identity must be supplied by the eventual analysis
job integration. Aliased GPA identities still need explicit integration tests.

## Verified results

The final complete matrix passes 1,353 Qt checks with zero failures and skips in
338.343 seconds, producing 1,350 GPU case reports across WARP, NVIDIA and Intel.
Six independent report validators pass:

| Matrix | Cases | Rejected corruptions |
| --- | ---: | ---: |
| Live snapshot identity and selection | 1,350 | 10,800 |
| Single-sample merger | 546 | 4,914 |
| MSAA merger | 144 | 2,016 |
| Color coordinate merger | 144 | 1,872 |
| Depth coordinate merger | 72 | 936 |
| Depth-only merger | 432 | 9,072 |

The validators reject 29,610 corrupted reports in total; the snapshot validator
and numerical validators deliberately check overlapping GPU cases. The separate
historical compatibility check adds 432 old cases and 7,776 corruptions, excluded
from these totals.

Release builds of the GUI, CLI, Worker and affected test executables pass. Four
supporting CTest entries pass serially in 23.18 seconds: `native_history`,
`texture_inspector`, `texture_edit_replay` and `predication`. The retained Qt logs
report 91, 34 and 43 checks for the three suites that emit per-case logs; the
texture inspector entry has no per-case output in the CTest log.

Suite elapsed time includes repeated capture creation, device creation, compiler
work and reference replay. It is not an isolated job latency comparison or a
final P8 performance benchmark. No new transient module-load or peak-memory
audit was performed for this increment.

## Reproduction

Build `FloraNativePixelMergerTests` and the relevant replay consumers in Release.
Run GPU tests serially and use fresh output paths:

```powershell
$env:FLORA_PIXEL_MERGER_EVIDENCE = "$PWD/artifacts/dx11-native/merger-snapshot-rerun"
ctest --test-dir build/vs2022 -C Release -R '^native_pixel_merger$' --output-on-failure
python tools/validate_native_merger_snapshots.py "$env:FLORA_PIXEL_MERGER_EVIDENCE" --out snapshots.json
python tools/validate_native_pixel_merger.py "$env:FLORA_PIXEL_MERGER_EVIDENCE" --backend native-pixel-merger/4 --out single.json
python tools/validate_native_pixel_merger.py "$env:FLORA_PIXEL_MERGER_EVIDENCE/msaa" --backend native-pixel-merger/4 --msaa --out msaa.json
python tools/validate_native_pixel_merger.py "$env:FLORA_PIXEL_MERGER_EVIDENCE/coordinates" --backend native-pixel-merger/4 --coordinates --out coordinates.json
python tools/validate_native_pixel_merger.py "$env:FLORA_PIXEL_MERGER_EVIDENCE/depthCoordinates" --backend native-pixel-merger/4 --depth-coordinates --out depth.json
python tools/validate_native_depth_only_merger.py "$env:FLORA_PIXEL_MERGER_EVIDENCE/depthOnly" --out depth-only.json
```

The adjacent evidence ledger pins sources, executable snapshots, reports and
logs. Original captures and large raw artifacts remain outside Git. Full merger
integration, original-fragment qualification, debugger parity and P7/P8
dependency-removal acceptance remain open.
