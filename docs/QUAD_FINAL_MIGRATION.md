# Native final-geometry submission for Quad diagnostics

`application/QuadFinal.cpp` implements the original `quad_post_transform.py`
helper. It complements `QuadSerial`: original HS/DS/GS/SO output, adjacency and
pre-raster UAV writers can be captured before isolated diagnostic submission.
The full counter executor and Qt Quad panel remain pending. The delivered GUI
package remains `out/FloraGPA-coverage-ui/FloraGPA.exe`.

## Capture and rasterization contract

`captureBoundPostTransform` exposes the existing native final-stage SO capture
inside a caller-owned bound input/experiment scope. It returns the original raw
capture report, without export/table fields, and does not replay the prefix or
submit/count an original event. Existing PostTransform inspection remains on
its completed-before-event entry point.

Quad selects the original GS rasterized stream, including SO signature-only
providers, and rejects `NO_RASTERIZED_STREAM`. The original WARP direct-DS
isoline rejection remains explicit; final geometry export is still available.
The existing bounded SO capture provides private outputs/UAVs and counters,
retry accounting, experiment shaders, disabled-event behavior and SO cursor
preservation. This implementation does not introduce a second capture engine.

The generated VS and GS carry only the original rasterizer system outputs:
position, clip/cull distances, render-target array index and viewport index.
Semantic layouts and storage bounds are validated. Float4 position is required.
The GS emits position at register 0 and a local zero PrimitiveID at register 1,
ahead of clip/array fields, preserving the recovered diagnostic PS linkage.
Both OSGN and streamed OSG5 signature encodings use the checked DXBC reader.

Each captured point, line or triangle is submitted separately using the original
captured stride and attribute offsets. SO is unbound and HS/DS are disabled for
helper submission. Captured CPU bytes are released after creating the native
vertex buffer; only the original report and required GPU objects remain alive.
As in Python, the caller owns complete pipeline restoration and the surrounding
private-output pass. An empty/disabled capture issues no helper draw.

## Validation method and initial findings

`tools/validate_quad_final.py` executes unmodified Python `PostTransformCounter`
and the native helper against the same independently generated frames. It
compares complete capture/serialization metadata (including geometry byte
hashes), generated signatures, two repeated submission passes, full texture/SO
buffer hashes, UAV hidden counters, execution counts and rejection messages.
It separately requires that capture preserve original resources and that helper
passes preserve SO/UAV buffers and counters. Only three specifically named
runtime/stream rejection cases are allowed; an unexpected reference failure
does not count as a passing comparison.

Initial unit tests exposed two signature integration mistakes: reflection uses
the `output` key, while the original raw inspection uses `OSGN`; the generated
GS actually contains OSG5, which Python normalizes under OSGN. The native helper
now reads either actual DXBC chunk using the existing shared signature parser.
The original failures remain in `artifacts/quad-final-unit-v1.txt` and `-v2.txt`.
Position/PrimitiveID register validation was retained, not weakened.

The first 128-case GPU comparison matched all geometry, signatures, resource
storage, submission results and isolation checks. Forty-eight UAV cases differed
only because Python also counts the OM setter as `state_or_auxiliary_records`.
The verifier now independently counts those captured OM records before omitting
that decoded-record label; native execution counts are still compared. The raw
initial evidence remains in `artifacts/quad-final-v1/`.

The final matrix passes **140 hardware/WARP cases**, including three exact
expected rejections. It adds 2/4/8x MSAA with and without array routing, comparing
every color and depth/stencil sample before capture, after capture and after
each of two helper passes. Full expected/actual JSON, source hashes and the
probe hash are retained in `artifacts/quad-final-final/validation.json` and its
adjacent files. This is evidence for the native helper, not the pending complete
counter executor.

Other cases cover tessellation points/lines/triangles, generated GS lists/strips,
clip/cull distance, viewport routing, all four rasterized GS streams, VS/DS and
signature-only SO providers, dynamic VS/HS/DS/GS class instances, pre-raster UAV
slots 1/8/63 with and without hidden counters, disabled draws and edited DS code.
SO cases inspect the second producer draw, so original buffers already contain
live data and native append history before diagnostic capture.

The complete Release build passes (`artifacts/build-quad-final-full.log`). Ten
CTest suites pass: `quad_final`, `quad_serial`, `dxbc_quad`, `post_transform`,
`geometry`, `stream_output`, `class_linkage`, `coverage`, `coverage_ui` and `core`
(`artifacts/ctest-quad-final-final.log`). The new unit suite has three passing
cases including setup/cleanup, with no skips; it checks real hardware/WARP
triangle rasterization, repeated submission and preserved color/depth storage.

Four independent PostTransform export comparisons pass on hardware/WARP
(`artifacts/quad-final-post-transform/validation.json`). GF2/BF1 golden pixel
hashes, execution counts, suppress-draw negative controls and runtime module
audits pass with the updated Release CLI under isolated child process paths
(`artifacts/quad-final-golden/validation.json`).

The ledger marks `quad_post_transform.py` as `ported`: its constructor, submit
method, metadata and generated output signature responsibilities are migrated.
Counts are 23 ported, 117 partial and 64 pending. These module counts are not a
workload completion percentage, and the full Python-to-C++/Qt goal is unfinished.

## Remaining integration

Target/view selection and resource helpers are now implemented separately in
[QUAD_RESOURCE_MIGRATION.md](QUAD_RESOURCE_MIGRATION.md). Both serial strategies
still need integration with private passes and UAV slot restoration, depth preparation, counter/reference
programs, uint32 artifacts/report/preview and Qt controls. The original event
must run exactly once after diagnostics. Splitting changes scheduling; these
helpers do not measure physical quad invocations and do not reconstruct GPA's
original allocation or scheduling policy.
