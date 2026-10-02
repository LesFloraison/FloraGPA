# M2: pipeline setters at captured event boundaries

Ordinary captured shader and fixed-function pipeline setters now execute at their
own event. Previously, an unedited setter was skipped unless it reset an active
experiment; a subsequent Draw/Dispatch snapshot supplied the state. This could
produce a correct final image while exposing stale native state before that draw.

## Scope and missing information

The existing checked decoder covers six shader stages, primitive topology,
blend state/factors/sample mask, depth-stencil state/reference, rasterizer state,
viewports and scissor rectangles: twelve legacy immediate-context record types.
They now use the same native binding path regardless of whether an experiment is
active. ClearState and captured setters still terminate experiment overrides.
Strict length, optional-array, immediate-context, resource-type and value checks
apply in both preflight and replay. Linked records remain explicitly unsupported.

Draw snapshots still restore state omitted from the captured command stream. This
change does not claim the command stream alone contains every state transition,
nor extend the twelve layouts to other context interface versions.
The existing native viewport path requires feature level 11_0 or higher.

BF1 demonstrates a real missing-information boundary: VSSetShader events 26972
and 27675 refer to absent shader 26973; PSSetShader events 26974 and 27676 refer
to absent shader 26975. Preflight reports `shader_binding_not_saved` with both
event and resource IDs. Replay retains an unresolved binding until a later valid
setter, ClearState, or successfully applied draw snapshot. Stopping in that gap
is rejected; the native-state inspector cannot present the previous shader as
the requested state. Unknown shaders with class instances and wrong resource
types are rejected rather than accepted as this narrow missing-identity case.
No shader is synthesized or modified. This preserves complete-frame playback of
the original BF1 while honestly limiting intermediate-state inspection.

`pipeline_setter_records` counts actual executed setters; `unresolved_shader_setters`
counts the missing-identity cases. The legacy auxiliary-count field is retained
for existing report compatibility, not as the capability classification.
GPU-statistics compatibility reports omit the new diagnostic counters while raw
engine reports retain them.

## Independent evidence

`FloraPipelineBoundaryProbe` is an opt-in development target reusing the self-owned
buffer producer. It adds all twelve setters, zero and nonzero viewport/scissor
arrays, default blend factors and a nonzero stencil reference before the
buffer-dependent draw. Native getters independently check the zero-viewport,
blend-factor/sample-mask and stencil-reference boundaries. Green GPU output still
depends on actual buffer input; a magenta preclear detects omitted drawing.
Each of five modes runs twelve frames both natively and under the original GPA
shim, producing unmodified original captures kept outside Git.

`PipelineSetterTests` checks captured boundaries with no experiment or draw
snapshot to conceal a skipped call. It compares native COM identities and scalar
or array values, tests hardware/WARP and repeat replay, and covers all twelve
record layouts' strict prefixes, trailing bytes and linked records. Additional
cases reject invalid topology/context/resource/class references and exercise
missing-shader recovery through a setter, ClearState and before-draw snapshot,
including an overridden shader followed by a missing captured binding.
The original-capture test checks every pipeline setter in the three drawing modes
on hardware and WARP. HS/DS/GS/CS in these new originals use null shaders; this
does not independently certify every non-null stage, dynamic linkage or driver
combination. Existing shader-edit/class-linkage and game regressions remain
separate evidence.

The native contracts are documented by Microsoft's
[VSSetShader](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-vssetshader)
and [OMSetBlendState](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetblendstate):
null shader bindings disable the stage; a null blend-factor pointer stores four
ones, even when blending itself does not use them.

```powershell
cmake --build --preset release --target FloraPipelineBoundaryProbe FloraPipelineSetterTests
python tools/capture_buffers.py --pipeline-boundaries --producer build/vs2022/Release/FloraPipelineBoundaryProbe.exe --out artifacts/new-pipeline-originals
$env:FLORA_PIPELINE_CAPTURES = "$PWD/artifacts/new-pipeline-originals"
ctest --preset release -R '^pipeline_setters$' --output-on-failure
```

## Acceptance integrity and remaining scope

During the first full corpus run, BF1 failed at its missing shader identity.
The batch JSON correctly recorded that failure, but the batch exit status was
zero. The development validator now fails for recorded native replay failures,
blocked/failed preflight, requested original-player/export failures, failed
controls/boundaries and runtime dependency violations. An empty run cannot pass.
Warnings and measured nondeterminism remain distinct from these failure gates;
no global image threshold was relaxed. Regression tests check the failed BF1
run as well as the previous passing corpus.

The new producer's getter checks exposed two additional unsupported records:
`OMGetBlendState` (`0x3539`) and `OMGetDepthStencilState` (`0x353a`). Read-only
Ghidra decompilation of the installed original `shimd3d64.dll`, RVAs `0x132230`
and `0x132b40`, confirms their wire layout: common 16-byte prefix, returned state
identity, optional four-float blend factor and optional UINT mask, or optional
UINT stencil reference. Their strict metadata decoders preserve output-pointer
presence and reject truncated records, trailing bytes and non-Boolean flags.
Returned IDs/values do not set replay GPU state. A zero state ID cannot distinguish
an omitted output pointer from a null returned binding. Tests cover every optional
combination, opaque returned identities and unchanged native bindings. The original
producer capture that first failed is retained, not rewritten to remove these calls.

This work continues M2. Deferred execution, resource versioning, unsupported
interface layouts, missing resource bytes and M3–M6 remain incomplete. The
module migration totals remain 72 ported, 117 partial and 15 pending.

## Final acceptance results

The immutable originals are registered in [pipeline-corpus.json](pipeline-corpus.json).
Binary/source hashes and retained result paths are recorded in
[pipeline-setter-baseline.json](pipeline-setter-baseline.json). The final package
is `out/FloraGPA-pipeline-final-20261003/`.

| Check | Result |
|---|---|
| Historical 91 files | 182 independent runs; 90 stable images unchanged; Helldivers retains documented variability |
| Prior five Present and five buffer files | 20 runs; all stable images unchanged |
| Five new original pipeline files | 10 independent and 10 original-player runs; exact matching images |
| Default runtime audit | All 212 independent runs exclude GPA/Python/RenderDoc modules |
| Corpus controls | 15 passed, including six new draw-disabled magenta controls and retained Helldivers controls |
| Isolated packaged goldens | GF2/BF1 and both draw-suppressed controls passed |
| Final CTest | Nine suites passed; 55 main UI and nine pipeline Qt cases, zero skips |
| BF1 gap rejection | Stopping at event 26972 rejects absent shader 26973; full-frame golden remains unchanged |
| Development harness | Ten CPU tests passed; failure exit-gate regression also checked against retained failed and passing real corpus reports |

GPU tests and replays ran serially. The original-player adapter remains unidentified;
the exact image matches do not establish matched-device/driver equivalence.
Native producer and C++ getter checks establish intermediate-state evidence;
original-player comparison establishes final-image evidence, not a full original
kernel state dump. All twelve families in the historical 99-type table now have
execution paths (59 execute, 40 metadata), but missing shader records still have
explicit recovery boundaries. Type counts do not certify every captured layout.

Earlier failed attempts remain in `artifacts/m2-pipeline-exploration` (undeployed
Qt dependency), `m2-pipeline-packaged` (BF1 missing shader), and
`m2-pipeline-original-failure` (new OM getter gap). Final acceptance uses the
`m2-pipeline-release-*` results, not those exploratory outputs. Ghidra ran against
the existing project in read-only mode; the original capture bytes were unchanged.
