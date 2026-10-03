# M2: geometry-stage and stream-output shader creation

This increment adds GPA 2025 R1 Device5 CreateGeometryShader (`3582`),
CreateHullShader (`3585`), CreateDomainShader (`3586`), and
CreateGeometryShaderWithStreamOutput (`3583`) to the checked pipeline-creation
path. Ordinary creation uses inline DXBC and verifies saved shader kind,
device, linkage and bytecode when a resource snapshot exists. Failed HRESULTs
and S_FALSE validation calls remain explicit metadata observations. Future
resource snapshots cannot make a shader available before its creation event.

## Stream-output format evidence

The original SO writer at RVA `e9a90` writes bytecode, a count and presence flag,
24 bytes per declaration entry, a strides-pointer presence flag, **only the
first stride**, the stride count, rasterized stream, linkage and returned
identity. Semantic names in these entries are opaque process pointers. The
saved resource references a complete type `9/85` declaration with string IDs
and all strides. Read-only evidence is in
`artifacts/m2-geometry-creation-wrappers.c`, with its Ghidra log and string RVAs.
Ordinary GS/HS/DS writers are at `e8f70`, `eb190`, and `ebcb0` respectively.

FloraGPA compares the observed declaration fields, null/non-null semantic
pointers, first stride, total stride count and rasterized stream to that saved
declaration. The saved strings and remaining strides supply the information
omitted by the API record. No process pointer is dereferenced and no missing
stride is copied from the first slot. Declaration bounds, component/slot ranges,
string bounds and malformed/truncated records fail explicitly.

An unused first SO shader in repeated-creation modes 33 and 43 has no resource
snapshot, hence no complete saved declaration. It is reported as unmaterialized
metadata with a per-event handling override and execution count. Audit rejects
any draw-state dependency on that identity, and runtime object access also
rejects it. The second SO shader is fully saved and executes normally. Ordinary
unused GS/HS/DS shaders do have enough inline bytecode to create an object even
without a resource snapshot. These cases are distinguished in diagnostics.

## Original fixtures and independent oracles

[geometry-creation-corpus.json](geometry-creation-corpus.json) pins twenty
unmodified original CaptureNextFrame files from the self-owned
`tools/native/geometry_creation_probe.cpp` producer.

| Modes | Behavior |
|---|---|
| 0–3 | Ordinary geometry shader passes a full-screen triangle |
| 10–13 | Hull shader creation with a real tessellation pipeline |
| 20–23 | Domain shader creation with a real tessellation pipeline |
| 30–33 | Geometry shader with SO and rasterization |
| 40–43 | VS bytecode used as the SO passthrough shader, with rasterization |

Every family has successful creation, failed then valid creation, validation-only
then valid creation, and two successful creations. Each producer run verifies
all twelve frames, natively and under the original shim: 480 frame checks.
All images are green 8x8 RGBA. The SO cases also check two complete output
buffers: three position vectors at stride 16 and three colors at stride 20,
including the untouched padding. Expected bytes are constructed independently
by the producer and retained alongside actual native/injected bytes.

The forty hardware/WARP replay cases verify object presence at creation events,
query native GS/HS/DS binding identity after Draw, and check
SO output bytes, declaration strides and explicit metadata exceptions. Negative
cases cover every truncated wire prefix, trailing bytes, invalid flags/counts
and lengths; original-derived mutations exercise bad record links, identity/device,
DXBC, semantic pointer, first stride, stride count, rasterized stream, resource
kind, truncated declarations, and use of a missing declaration. Moving creation
after use is rejected for all five families. Research mutations supplement the
unmodified original files and are never used as the sole acceptance evidence.

## Acceptance

All 231 registered files complete 462 independent replays: 230 repeat exactly,
while Helldivers retains its known variability. All 210 previously stable
image hashes remain unchanged. Forty new original-player comparisons, 227
corpus controls, 88 retained texture-boundary exports and four golden/negative
checks pass. All 462 ordinary runtime dependency audits pass; five packaged
binaries match Release. The preceding package rejected all twenty new files;
its preflight reports remain in `artifacts/m2-geometry-creation-before/`.

Twenty-eight related CTest suites pass. Geometry creation has 45 passing cases,
previous pipeline creation 70, class linkage 10, main UI 55 and texture edit
replay 34, without skips. Thirty-two hardware/WARP SO buffer dumps exactly
match the independently constructed producer bytes. A focused 45-case rerun
after adding native Draw-time shader binding assertions also passes; both test
runs are pinned in the baseline.

Final results and hashes are pinned in
[geometry-creation-baseline.json](geometry-creation-baseline.json).
The release package is `out/FloraGPA-geometry-creation-20261003/`.
Original captures, images, binary observations and generated logs remain outside
Git. GPA binaries and Python serve development comparisons only; runtime is
C++/Qt. The original player's adapter identity remains unknown, so byte-equal
images are observed agreement, not matched-device equivalence.

Build `FloraGeometryCreationProbe` explicitly. Its arguments are
`<new-output-dir> <mode> [<capture.gpa_frame> <shimloader64.dll>]`; use
`GPA_LOCAL_INJECT=true` for capture. Set `FLORA_GEOMETRY_CREATION_CAPTURES` to
the directory containing the numbered capture folders for CTest. Run GPU
checks serially. Set `FLORA_GEOMETRY_ARTIFACT_DIR` to retain the 32 hardware/WARP
SO buffer dumps (two buffers in eight SO cases on two devices). The corpus uses
the existing `validate_corpus.py` runner with
two repeats; `--oracle-tools` enables the original kernel only in development.

## Remaining scope

The new captures exercise two output slots, stream zero, rasterization, ordinary
GS bytecode and VS passthrough. They do not certify every SO declaration,
signature-only creation, multi-stream output, dynamic linkage or adapter. Existing
pre-frame SO regression tests cover additional cases without turning those into
claims about newly captured creation layouts. Class-linkage creation, extended
interface/version layouts, general identity/version reconstruction and deferred
execution remain open. Missing frame-before resources/counters remain separate
M4 work. M2 and M3–M6 are incomplete; module migration counts stay unchanged.
