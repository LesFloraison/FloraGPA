# Event texture edits

Native event texture input/output edits now use the same JSON operations as the
Python reference: `texture_input` and `texture_output`, with an embedded hashed
RAW asset, resource, event, mip and array layer. MSAA edits additionally require
an explicit sample and accept an optional compatible typed format.

The Texture toolbar exposes **Import Input…** and **Import Output…**. The dialog
uses the selected mip/layer, checks the exact file length, and records one undoable
operation. A 3D mip includes every depth slice, even when its output view covers
only a subset. No DDS header, display conversion or inferred sample data is used.

## Lifetime and storage

- Input patches clone current storage, redirect only the active graphics or
  compute SRVs, and restore resource/view identities afterward. Other-stage SRVs
  and output aliases retain original storage. Duplicate patches apply in history
  order. Input validation requires the resource to be bound, without requiring
  the edited subresource to be visible through its SRV.
- Output preconditions copy into original resources, preserving view identities.
  They apply before input clones and persist after actual command submission.
  Preview, disabled commands and pre-submit failures restore GPU backups. A
  failure reported after submission retains output edits, matching the reference
  command-count rule.
- Final setter and frame-wide view edits determine validation, independently of
  operation order. Output edits require a matching bound mip and array layer.
- Non-MSAA transfers cover 1D/2D/3D storage, mip arrays and cubes. Packed sizes
  include BC blocks and planar planes; GPU planar integration remains incomplete.
- MSAA transfer uses an isolated deferred-context draw and one-bit sample mask.
  Compatible UINT views preserve raw integer/float bits. Packed D24S8 and D32S8
  use separate depth and stencil passes. D32S8 padding/subnormals, R11G11B10 NaN
  payloads and BGRX padding are validated before writing. Internal draws isolate
  active predication and restore the immediate context.

## Evidence

- `tools/validate_texture_edit_storage.py`: 3,229 Python storage/encoding cases,
  including 1,298 accepted and 1,931 rejected inputs.
- `tests/TextureEditTests.cpp`: explicit layouts, checked size arithmetic,
  MSAA byte restrictions, active-stage inputs and output subresource matching.
- `tests/TextureEditReplayTests.cpp`: 32 hardware/WARP cases for native storage,
  untouched samples/layers, inactive SRV aliases, before/after scope, disabled
  events, failures before/after submission, project save/load and undo/redo.
- `tools/validate_event_texture_port.py`: 180 byte-exact Python comparisons
  using actual graphics/compute commands. RTV, CS/OM UAV, DSV (including read-only
  views), 1D/2D/3D/cube resources, combined input/output aliases, reversed history
  order, MSAA color/depth inputs and outputs are covered.
- `tools/validate_event_texture_real.py`: GF2 input edit and restoration through
  native CLI storage/preview, edited final output and undo against Python.
- `tests/UiTests.cpp::textureEditorHistory`: compact RAW import, invalid-file
  rejection, native worker preview, undo/redo and after-event scope.

Validation artifacts are local under `artifacts/`; captures are not distributed.
Tests do not establish support for every DXGI format/sample combination on every
adapter. D3D11 capability errors remain explicit.

Release evidence for this batch:

| Check | Local artifact |
|---|---|
| 33 CTest suites passed | `artifacts/ctest-event-texture-final.log` |
| 51 Qt cases, no failures or skips | `build/vs2022/ui-Release.txt` |
| CPU reference comparison | `artifacts/texture-edit-storage-final/validation.json` |
| GPU reference comparison | `artifacts/event-texture-port-final/validation.json` |
| Real GF2 edit and native runtime audit | `artifacts/event-texture-real-package/validation.json` |
| Packaged GF2/BF1 golden and suppressed-draw controls | `artifacts/validation-event-texture-package/validation.json` |
| Rendered Qt import and edited texture views | `artifacts/event-texture-ui/` |

The portable directory is `out/FloraGPA-event-textures/`. Its CLI and worker share
SHA-256 `93a3e8396b56cc4094fe17b149ad0cf07f318cfa387c53b4c363d450e844c31d`;
GUI SHA-256 is `736c36b00ef9f91c91457366200001f269f5abb9d545e4d38fd13ddeecbba6bf`.
Testing was on this host, not a separate clean Windows machine.

## Remaining migration work

The Texture pane now includes native MSAA/typed/planar inspection and DDS/RAW/PNG
export controls. See [TEXTURE_INSPECTION_MIGRATION.md](TEXTURE_INSPECTION_MIGRATION.md)
for subsequent inspection evidence and captured-Y boundaries. Planar write provenance
is covered by [PLANAR_WRITE_MIGRATION.md](PLANAR_WRITE_MIGRATION.md).

Private coverage, quad/debug/profiler consumers and the remaining module inventory
also remain incomplete. `docs/migration.json` retains partial status; this change
does not claim full Python application parity.
