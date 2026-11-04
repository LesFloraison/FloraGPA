# M2: captured shader, input layout and pipeline state creation

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

> **Subsequent work:** [Geometry/SO creation](GEOMETRY_CREATION_AUDIT.md) subsequently added GS/HS/DS and SO paths; [class creation](CLASS_CREATION_AUDIT.md) added linkage/instance paths with an explicit original-player disagreement. Earlier pending lists below remain batch-specific.

This increment executes eight GPA 2025 R1 Device5 creation record families at
their captured events. It does not certify other interface/version layouts.

| Record | Native operation | Original fixture modes |
|---|---|---|
| `3581` | CreateVertexShader | 0–3 |
| `3584` | CreatePixelShader | 10–13 |
| `3587` | CreateComputeShader | 20–23 |
| `3580` | CreateInputLayout | 30–33 |
| `3589` | CreateBlendState | 40–43 |
| `358a` | CreateDepthStencilState | 50–53 |
| `358b` | CreateRasterizerState | 60–63 |
| `358c` | CreateSamplerState | 70–73 |

Each group has success, failed-then-valid, S_FALSE-validation-then-valid and
repeated-creation cases. Failed and validation-only observations do not allocate
objects; preflight exposes these per-event metadata exceptions. Successful
shader creation consumes the saved inline DXBC, checks saved bytecode/linkage
when a resource snapshot exists, and preserves shader experiment overrides.
Input layouts use the saved semantic strings, numeric elements and full input
signature. Observed process pointers are never dereferenced or guessed.

## Identity, normalization and missing strings

The original probe's repeated shader/layout calls return distinct identities.
The first unused shader has no separate resource snapshot, but its creation
record contains complete DXBC and can therefore be executed. Created shaders
and layouts cannot be read from a future snapshot before their creation event.

Repeated state creation in all four original state families returns the same
identity. An immutable state may already be in use before another Create call
returns that identity. FloraGPA permits that case, creates from the captured
original descriptor, compares native GetDesc with the saved canonical descriptor,
and verifies cached IUnknown identity if the object is already materialized.
The C++ tests separately exercise use before creation and repeated creation.

Canonical descriptions need not equal the original input bytes. The point
sampler's original MaxAnisotropy is 1 while its saved GetDesc reports 0. Blend
state snapshots use DESC1 and rasterizer snapshots use DESC2. Comparison uses
active blend semantics, ignores structure padding, and checks the applicable
canonical state fields. An inconsistent saved descriptor fails explicitly.
These observations are fixture evidence, not a universal normalization rule.

Mode 33's first unused input layout has no saved resource or semantic strings.
The API record contains opaque pointers, which cannot recover those strings.
Its creation is explicitly reported as unmaterialized metadata, including a
per-event handling override and execution count. Audit verifies no saved draw
state requires that identity; any runtime request for it is rejected. The
second layout has complete saved semantics and is created normally. A mutation
making a draw state reference the first layout is rejected by preflight.
This exception reports missing capture information; it does not synthesize it.

## Original evidence and tests

[pipeline-creation-corpus.json](pipeline-creation-corpus.json) pins 32 unmodified
original CaptureNextFrame files. The self-owned C++ producer verifies twelve
frames both natively and under the original shim: 768 frame checks. Modes use
observable behavior: input-layout DATA semantics, blend factor, GREATER depth,
a scissor rectangle, border sampling, and a compute-written uint value of 123.
Native and injected images, compute bytes, device observations and producer
source/executable hashes are retained with each capture. The rasterizer image
has a green left half and magenta right half; other cases produce green.

The original writer decompilation is retained read-only in
`artifacts/m2-next-pipeline-creation-wrappers.c`, with its Ghidra log and string
RVA list. Parsing uses checked presence flags, lengths, record termination,
resource kind/device and linkage references. Synthetic cases reject every
truncated prefix, trailing bytes, invalid flags/results and excessive lengths.
Original-derived negative cases alter references, descriptor presence, DXBC,
semantic pointers, resource kind, canonical state or event order. These research
mutations supplement the unmodified original acceptance captures.

Seventy PipelineCreation Qt cases pass without skips. Sixty-four exercise the
32 original captures on hardware and WARP, inspect objects immediately after
creation, check repeated cached identity and compute bytes, and distinguish
unused inline shaders from the unmaterialized layout. The shader factory is
shared with pre-frame resource loading, preserving reflection, stripped shader,
class linkage and stream-output behavior; those paths receive regression tests.

## Reproduction and acceptance

Build `FloraPipelineCreationProbe` explicitly. Arguments are
`<new-output-dir> <mode> [<capture.gpa_frame> <shimloader64.dll>]`;
set `GPA_LOCAL_INJECT=true` for capture. Set
`FLORA_PIPELINE_CREATION_CAPTURES` to the directory containing the numbered
capture folders for the CTest suite. Run GPU work serially. The corpus runner
uses `tools/validate_corpus.py`, the committed manifest, two repeats and the
original-player adapter supplied through `--oracle-tools` in development only.

The packaged release is `out/FloraGPA-pipeline-creation-20261003/`.
All 211 files complete 422 independent replays: 210 repeat exactly, while the
known Helldivers variability remains. All 178 previously stable image hashes
are unchanged. The new files pass 64 original-player comparisons; 187 corpus
controls, 88 retained texture-boundary exports and four golden/negative checks
pass. Twenty-seven related CTest suites pass, including shader setters, class
linkage, stream output, sampler/blend/rasterizer and the earlier creation paths.
The initial run passed 26/27 suites. ClassLinkage's old boundary test still
expected CSSetShader event 90 to be ignored, although commit `9306d5e` already
executes captured pipeline setters. Its expectations now verify bound shader and
class at 90, explicit unbind at 95, and Dispatch snapshot restoration at 100.
The corrected ClassLinkage suite passes all ten cases; its first failure and
corrected run are both pinned. No runtime change was needed for that correction.
The new suite has 70 cases and main UI 55, without skips. All 422 ordinary
replays pass runtime module audits; five packaged binaries match Release.
All 32 new files were rejected by the preceding package; those preflight reports
remain in `artifacts/m2-pipeline-creation-before/validation.json`.

Release acceptance results are recorded in
[pipeline-creation-baseline.json](pipeline-creation-baseline.json). Capture files,
original binaries, generated images and logs remain outside Git. The original
player's adapter is unidentified; matching images are observed agreement, not
strict matched-device equivalence. The production runtime remains C++/Qt and
has no GPA or Python dependency.

## Remaining boundary

This increment does not implement frame-time GS/HS/DS or stream-output shader
creation, class-linkage creation, extended device creation layouts, general
object identity/version reconstruction or deferred command-list execution.
Existing pre-frame GS/HS/DS/SO handling is retained, not newly certified as a
creation path. Frame-before resource/counter recovery remains a separate gap.
The captured input layout exception cannot recover absent semantic strings.
M2 and M3–M6 remain incomplete; migration module counts are unchanged.

Subsequent acceptance: [GEOMETRY_CREATION_AUDIT.md](GEOMETRY_CREATION_AUDIT.md)
adds ordinary GS/HS/DS and GS-with-SO creation, including two output slots and
VS passthrough. The earlier 32 fixtures remain in its regression matrix.
Class-linkage creation, broader interface layouts and deferred execution remain
open; the historical limitations above describe the preceding release.

## Zero-element input layouts (2026-11-04)

The local capture `frame-46751be7-307c-4a5f-97ab-ba1a110bdd9a.gpa_frame`
(SHA-256 `d0605d21e7084f537807bfccdf34236d9f1769ff99b43c21395276186434236f`)
failed at DrawInstanced 1186 while restoring input layout 78. Its saved layout
has zero elements and an SV_VertexID-only input signature. D3D11 accepts this
layout but rejects a null descriptor pointer even when the element count is zero.
Both snapshot restoration and captured CreateInputLayout now pass a non-null
placeholder while preserving count zero, native signature validation and object
identity. No vertex element or shader input is fabricated.

Eight hardware/WARP cases cover both creation paths, a rendered red pixel and
bound object identity, and rejection of a signature requiring an absent POSITION.
All four valid cases failed before the fix; all eight now pass. Six related CTest
suites pass serially. External-fixture skips remain (66 in pipeline_creation).
The three targeted UI tests pass, including RGB visibility for zero-alpha output,
explicit RGBA/alpha inspection, capture reset and project display restoration.
New captures default to RGB so a final color buffer with zero alpha remains visible;
captured storage and explicit channel selections are unchanged.

The unmodified capture completes on NVIDIA RTX 5060 Ti and WARP at 1920 x 1080,
with 241 draws and 46 dispatches. The packaged GUI completes automatic replay
and displays the frame. Hardware/WARP hashes differ; cross-device pixel parity
is not claimed. Four existing unsaved query-completion notices remain. BF1's
normal/negative hashes are unchanged from the preceding package; its documented
historical golden mismatch remains, and GF2 was unavailable on this host.
Local logs, images and reports are in `artifacts/frame-46751be7-diagnosis/` and
`build/empty-layout-*.log`; the package is `out/FloraGPA-dx11-fix-20261104/`.
This bounded fix does not change module status or establish full capture parity.
