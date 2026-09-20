# Persistent pipeline setters

The existing API Log **Edit Setter** action now covers these recovered command
arguments, using compact resource selectors, numeric fields and array tables:

| Command | Editable values |
|---|---|
| IASetPrimitiveTopology | Topology, including adjacency and 1–32 control point patches |
| RSSetState | Captured base/extended rasterizer resource or default |
| RSSetViewports | Zero to 16 float32 viewport rows |
| RSSetScissorRects | Zero to 16 signed int32 rectangles |
| OMSetBlendState | Captured base/extended blend resource, four factors, uint32 sample mask |
| OMSetDepthStencilState | Captured depth/stencil resource and raw uint32 stencil reference |

Edits use the existing `setter` experiment history operations and frame hash.
They apply at the original command and persist across later draw/dispatch state
snapshots. A later unedited setter of the same family applies its captured
arguments and ends that override; ClearState ends all active overrides. As in
the Python reference, these six unedited setter families remain dormant when
there is no active override. This preserves the recovered snapshot replay model.

An empty viewport/scissor array is explicit state. It is distinct from an absent
override and is preserved when a later snapshot is prepared. Before-event frame
output retains the previously established ordinary traversal; Replay State and
input consumers still prepare a selected draw snapshot.

## Dependent consumers

The effective state passed to IA geometry includes edited topology. Stream-output
accounting and DrawAuto use the same effective state and treat persistent setter
changes as edited execution. Per-draw rasterizer, blend and depth/stencil
experiments inherit the final preceding setter state, even when the per-draw
history operation was added before the setter operation. Existing editors read
these inherited defaults instead of presenting the original snapshot values.

Resource IDs retain uint64 precision. Setter validation requires a fully decoded
record and an immediate context, checks every required key, object family,
integer bounds, finite float32 conversion, viewport limits and rectangle order.
The wire decoder retains null-blend-factor defaults and validates array presence,
count and length. Inactive malformed captured setters follow the reference's
dormant behavior; an active override cannot silently disappear at an invalid
captured reset.

The GPU replay layer uses typed state and direct D3D11 calls. JSON normalization,
experiment persistence and the Qt editor remain outside it. No Python runtime is
introduced.

## Scope and verification

Package: `out/FloraGPA-pipeline-setters/FloraGPA.exe`.

- `artifacts/ctest-pipeline-setters-release.log`: all 28 Release suites passed;
  the Qt UI suite reports 43 passes and no skips.
- `artifacts/pipeline-setters-complete/validation.json`: 269 checks passed,
  including a 748-case wire/argument corpus (130 accepted, 618 rejected),
  hardware/WARP images and getter fields, reset/dormant behavior, geometry tables
  and OBJ, and the SO-to-DrawAuto path with an edited consumer topology.
- `artifacts/pipeline-setters-real/validation.json`: all 9 GF2 and 208 BF1 viewport
  setters edited; both frames match Python and visibly differ from the original.
  Undo restores each original golden hash. Native children use Windows-only PATH.
- `artifacts/validation-pipeline-setters-golden/validation.json`: both original
  golden frames and both suppressed-draw negative controls pass from the package.
- `artifacts/pipeline-setters-ui.txt` and `artifacts/pipeline-setters-ui/`: Qt
  validation, uint64 resource selection and worker Apply/Undo/Redo pass; editor
  and workspace screenshots were visually inspected.
- `artifacts/pipeline-setters-audit.json`: all three packaged executable hashes
  match the Release build; 274 successful native reports contain no Python, GPA
  or RenderDoc modules. All loaded Qt libraries come from the package itself.

The first differential run found that persistent blend setters accept finite
float32 factors outside [0,1], unlike per-draw blend experiments. That validation
was separated; the final corpus and native getter/pixel checks include negative
and large finite factors. The initial failure remains in
`artifacts/pipeline-setters-comparison/cpu-differences.json`.

`tests/PipelineSetterTests.cpp` covers persistence, native getter boundaries,
same-family reset, ClearState, project round trip, undo/redo, rejected-operation
atomicity and per-draw inheritance. `tools/validate_pipeline_setters_port.py`
compares the independent Python implementation's wire/argument validation,
rendered pixels, raw storage, output metadata and complete pipeline fields on
hardware and WARP. Its `--real-only` mode edits all recovered viewport setters in
GF2/BF1 and compares both edited and undone frames.

CB/CB1 setter migration is documented in
`docs/CONSTANT_BUFFER_SETTER_MIGRATION.md`. IA resource setter migration is documented in
`docs/IA_SETTER_MIGRATION.md`. Six-stage shader setter migration is now
documented in `docs/SHADER_SETTER_MIGRATION.md`. Pending coverage/debugger/private consumers
still require their own migration and verification.

```powershell
python tools/validate_pipeline_setters_port.py `
  --reference D:/CDXrepo/FloraGPA `
  --exe out/FloraGPA-pipeline-setters/FloraGPA.Cli.exe `
  --oracle build/vs2022/Release/FloraPipelineSetterTests.exe `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin `
  --out artifacts/pipeline-setters-comparison-new
```

For packaged real-frame checks, add `--real-only` and omit `--qt-bin`; the oracle
executable is unused in that mode.
