# Global view experiments

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

> **Subsequent work:** Main output selection, typed/MSAA inspection and event texture editors were subsequently connected. See [frame output](FRAME_OUTPUT_MIGRATION.md), [texture inspection](TEXTURE_INSPECTION_MIGRATION.md), [event textures](EVENT_TEXTURE_MIGRATION.md) and [planar writes](PLANAR_WRITE_MIGRATION.md) for their distinct scopes.

The native implementation now accepts the Python project's `view` operations
for SRV, RTV, DSV and UAV resources. This is part of the full migration;
view-related consumers are not all finished.

## Implemented

- Strict uint32 descriptor normalization, all recovered dimensions, flag rules,
  dimension-dependent fields and wire packing. Dimension changes discard fields
  outside the new union and require its remaining fields.
- Immutable replacement descriptors over shared read-only capture storage.
  Capture hashing and raw captured payloads remain original. Each effective frame
  has separate binding/context caches and survives destruction of its source.
- Global descriptors are assembled before validating other experiment operations.
  Project load/save, merged history, undo/redo and rollback on invalid edits use
  the same effective frame. Reapplying against an older edited frame first strips
  those old changes, so undone fields cannot leak into the next replay.
- Native view creation, Clear/Draw/Dispatch, GenerateMips, output/SRV hazards,
  counter inspection/edits, buffer binding validation and replay pipeline getters
  use the edited descriptors. Replay pipeline metadata identifies global edits.
- Qt **Edit > Edit View Resource**: view selector, dimension/format, contextual
  mip/layer/depth/element fields and flags. No large explanatory text in the UI.
  Switching resources requires Load; changed capture/history or a busy worker
  prevents stale commits. The existing event SRV editor remains independently
  scoped to its event.
- Non-MSAA `readTexture` and CLI `texture-storage` export tight raw storage for
  all array/mip/volume subresources supported by the existing storage format
  decoder. WARP inspection copies temporarily detach OM render targets and
  restore them while retaining UAV bindings and hidden counters.

## Validation

- `artifacts/view-codec-integration/validation.json`: **9,155** descriptor cases
  match Python, including 46 GF2 and 800 BF1 view records, all recovered dimensions,
  uint32 boundaries, malformed sizes/fields, padding and dimension changes.
- `artifacts/view-replay-complete/validation.json`: **358** checks pass on hardware
  and WARP. Shader-visible SRV ranges/formats, RTV/DSV/UAV 1D/2D/3D storage,
  typed/raw/structured buffer UAVs, Append/Counter/Consume ranges, read-only depth
  and stencil, sRGB clears, buffer RTVs, GenerateMips and invalid native descriptors.
  Both operation orders for global/event SRV edits produce the same result.
- The same runner compares GF2/BF1 global SRV mip changes and RTV sRGB changes
  against Python, and verifies unedited replay against the exact original hashes.
- `view_edits` CTest passes all **13** cases, including overlay lifetime, project
  history/load/save/rejected-edit rollback, event composition and repeated storage
  reads preserving RTV holes and UAV counters.
- `artifacts/view-ui-initial.txt`: both new Qt tests pass, covering four view kinds,
  stale selection, flags validation, rejected stale commits, menu-to-worker replay
  and undo/redo. Screenshots in `artifacts/view-ui-initial/` were visually reviewed.
- `artifacts/global-view-audit.json`: 244 successful native reports,
  74 loaded-module paths without Python/GPA/RenderDoc, and all three packaged
  executable hashes match Release. The package is `out/FloraGPA-global-views`.

- `artifacts/ctest-view-release.log`: **26/26** full Release CTest suites pass,
  including **37** Qt UI cases with no skips.
- Debug: the same **9,155** codec comparisons pass
  (`artifacts/view-codec-debug/validation.json`), **13** view tests pass
  (`artifacts/ctest-view-debug.log`) and both new Qt tests pass
  (`artifacts/view-ui-debug.txt`).
- `artifacts/validation-global-views-golden/validation.json`: the independent
  package passes both GF2/BF1 golden frames and both suppressed-draw negative
  controls with a Windows-only child PATH. The six real-capture edited/original
  comparisons also pass using only packaged dependencies
  (`artifacts/global-views-package-real/validation.json`).

The comparison runner uses Python only in development; native executables do
not load or launch Python or Intel GPA.

## Remaining scope

- Main frame-output selection and typed conversion do not yet follow every
  edited view's mip/layer/3D slice/aspect. The raw storage and native getter checks
  establish actual writes; they are not a claim of complete main-image parity.
- Private coverage/quad/debug consumers and texture input/output experiments
  remain dependent migration work. Their cross-feature view checks are pending.
- Dedicated MSAA output/sample inspection and some planar/packed storage formats
  are still pending. The raw storage API explicitly rejects multisampled input;
  existing MSAA SRV execution is tested separately through shader buffer output.
- The GUI's texture export workflow is not yet connected to raw storage export.
- External capture tests cover the supplied GF2/BF1 files on this host. They do
  not establish support for arbitrary captures or every GPU/driver.

## Reproduce

```powershell
# Run from the repository root; set external paths for your environment.
$QtRoot = 'C:/Qt/6.11.2/msvc2022_64'
$ReferenceRoot = 'C:/reference/FloraGPA'
ctest --preset release -R view_edits
python tools/validate_view_codec_port.py --reference "$ReferenceRoot" --exe build/vs2022/Release/FloraViewEditTests.exe --qt-bin "$QtRoot/bin" --out artifacts/new-view-codec --captures "$ReferenceRoot"
python tools/validate_view_replay_port.py --reference "$ReferenceRoot" --exe build/vs2022/Release/FloraGPA.Cli.exe --qt-bin "$QtRoot/bin" --out artifacts/new-view-replay --captures "$ReferenceRoot"
```

Always use a new artifact directory and run GPU checks serially. The advanced
counter fixtures are regenerated with an explicit immediate-context ID for old
synthetic CopyStructureCount records; both engines receive the same new fixture.
Original reference files remain untouched. UNORM half-value rounding can differ
by one unit between the hardware and WARP adapters; oracle comparisons require
exact bytes on the same adapter, including the unedited clear baseline.
