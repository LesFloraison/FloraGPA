# Native Texture inspection and export

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

> **Subsequent work:** Later consumer evidence includes [Coverage](COVERAGE_UI_MIGRATION.md), [Quad](QUAD_UI_MIGRATION.md), [shader recovery](HLSL_RECOVERY_MIGRATION.md) and [checkpoint UI](CHECKPOINT_UI_MIGRATION.md). Broad pending-consumer statements below are not a claim that those interfaces are absent today.

The Texture pane now inspects captured initial storage and before/after-event
storage through the native worker. Its compact controls select mip, array layer,
3D slice, MSAA resolve or sample, typed DXGI format, Y/UV plane, channel and display
range. Export saves DDS storage, the selected RAW subresource, or the displayed
PNG. MSAA input/output imports inherit the selected sample and typed format.

`TextureStorage` owns packed subresource/plane selection and DDS headers.
`TextureInspector` combines capture/replay storage with provenance and export.
`Replay::previewTextureStorage` performs display conversion on an isolated deferred
context; it preserves the immediate pipeline and isolates predicate queries.
Python is used only as a development comparison, never by the application.

## Storage and display semantics

- DDS and full RAW exports retain all mips and array layers, cube faces and volume
  depth. Selected subresource RAW retains every depth slice of that subresource;
  the slice selector controls the PNG preview.
- MSAA exports contain the resolved or selected-sample single-sample storage.
  Compatible integer paths preserve raw bits. The report states that pre-capture
  per-sample contents were not reconstructed; an unwritten region is not evidence
  of its original application value.
- NV12/P010/P016 planes are displayed as Y or U/V values without YUV-to-RGB color
  conversion. Plane extraction precedes GPU upload, so captured Y inspection does
  not require the adapter to create the original video texture format.
- Legacy GPA P010/P016 initial GenData has padded Y rows and unreliable UV.
  Initial inspection exports only recovered 16-bit Y as `luma.dds` / `plane.bin`,
  and preserves the original bytes as `capture_data.bin`. It explicitly rejects
  UV; it does not fabricate a complete planar DDS. Replay still requires a
  verified full DXGI replacement when such legacy initial data is present.
- Type 0x87 capture-end images may be inspected as captured assets. They remain
  prohibited as replay resources and are marked as reference pixels in the CLI
  inspection report.
- Failed selections clear the previous image and disable export, preventing an
  unavailable plane from appearing to reuse a successful earlier preview.

## Validation and a reference defect

`tools/validate_texture_inspector_port.py` compares DDS/RAW bytes, decoded PNG
pixels and texture/plane/MSAA metadata with the Python implementation on hardware
and WARP. It covers integer/normalized/float/sRGB/BC formats, typeless views,
1D/2D/3D/array/cube textures, legacy luma and explicit planar replacements,
MSAA color/depth and invalid selections, plus five real BF1 texture selections.
The final matrix passed all 217 cases: 178 successful exports and 39 explicit
rejections. Evidence: `artifacts/texture-inspector-port-final/validation.json`.

The Python preview concatenates a negative display minimum into an expression
such as `value--0.25`, which FXC rejects. Native shader generation now brackets
the literal. Successful range comparisons use a positive minimum, while
`TextureInspectorTests::msaaInspectionPreservesState` independently checks a
known UINT value of 32 in range [-32, 96]: every displayed RGB byte must be 128.
Matching Python/native compiler failures are not counted as successful range
rendering. The Python reference workspace has not been changed.

Qt tests exercise sample/layer/format/channel/range selection, actual DDS/RAW/PNG
file exports, import-dialog defaults, captured-Y export, and clearing/disabling
the view after an unavailable UV selection. Rendered evidence is retained under
`artifacts/texture-inspector-ui/`.

The Release build passed all 34 CTest suites (`artifacts/ctest-texture-inspector-final.log`).
The Qt suite passed 52 cases without failures or skips, including real capture
fixtures (`artifacts/texture-inspector-ui-final.txt`). All 180 event texture edit GPU comparisons still match Python
(`artifacts/event-texture-inspector-regression/validation.json`).
The packaged CLI also passed 15 real GF2 edit/runtime checks
(`artifacts/event-texture-inspector-real/validation.json`) and the GF2/BF1
golden frames plus suppressed-draw negative controls
(`artifacts/validation-texture-inspector-package/validation.json`).
The complete 217-case inspection matrix passed again using only the portable
package and Windows system paths (`artifacts/texture-inspector-package/validation.json`).
Successful runs audit loaded modules for Python/Tk/GPA/RenderDoc dependencies;
rejection cases require exit code 1 and a structured `completed: false` diagnostic,
so a process crash cannot satisfy the rejection check.

The portable application is `out/FloraGPA-texture-inspector/FloraGPA.exe`.
SHA-256 values for this build:

| Binary | SHA-256 |
|---|---|
| CLI | `c70c29bb0d4e7d1631b736b19dc1616e7b1f56f87d67cf39a30df6618597df4c` |
| Worker | `3135442aa74bcd001b846eb6fe1148a1652a5abb4ac954df8d402d0d06c8c47a` |
| GUI | `65ce749f10f4f1ee182be547d8f37cd480bade63c4a46f0ab426c69767dc42fa` |

## Remaining work

This closes the main Texture inspection/export controls, not the full migration.
Captured planar Map/Update semantics and their `planar_writes` reports were
subsequently migrated; see [PLANAR_WRITE_MIGRATION.md](PLANAR_WRITE_MIGRATION.md).
The Python `experiment` report field was subsequently migrated; see
[EXPERIMENT_REPORT_MIGRATION.md](EXPERIMENT_REPORT_MIGRATION.md). Private coverage, quad,
debugger/profiler consumers, full shader/source tooling and the remaining module
inventory also remain incomplete. These modules retain partial status.

All validation is on this host; separate clean-machine validation remains open.
