# Native post-transform geometry

The recovered `post_transform.py` path for Final, VS, DS and GS outputs now uses
native C++ D3D11 stream output. **Geometry** combines these stages with the
existing IA view, a stream selector (0–3), optional zero-based instance selection,
typed attribute rows, a rotatable wireframe and directory export. It runs in the
native Worker and respects the application's Hardware/WARP selection.

## Capture and state preservation

- Inspection replays to the selected draw's before boundary, prepares its captured
  state and applies the current experiment's inputs, shader replacements and
  pipeline settings. Disabled draws produce an empty, explicitly disabled result.
- Final output selects GS, then DS, then VS. Explicit VS selection disables HS/DS
  and inspects patch control points as points. DS requires the paired hull shader;
  explicit GS rejects an output-signature-only provider. Final mode supports such
  SO providers. Unknown or unpaired final topology is rejected.
- Output signatures retain float/int/uint types, packed semantic components,
  streams and offsets. Native primitive assembly handles strips and adjacency;
  results are expanded primitive vertices, not deduplicated IA vertices.
- Diagnostic GS/SO bindings disable rasterization and redirect writes to a
  private SO buffer. RTV/DSV/OM UAV owners and their active aliases use private
  resource copies, including hidden UAV counters. Copies are keyed by actual
  native resource identity so scoped input replacements cannot change which
  output storage is isolated. Original SO storage/cursors are retained.
- Each retry begins from the same private output snapshot. SO statistics select
  the required allocation; at most three attempts and 256 MiB of SO storage are
  allowed. Query waits are bounded. Bindings and original resources are restored
  on success and exceptions. Reported replay counts exclude diagnostic draws.
- Instance selection slices a full capture. Pure VS output has uniform primitive
  cardinality; generated HS/DS/GS output uses native prefix-count queries. Logical
  system IDs and IA offsets are unchanged. Generated-stage instance partitioning
  with any executed UAV declaration is explicitly rejected, including declarations
  detected directly in DXBC rather than relying on reflection.

These captures re-execute shaders. Atomic return ordering across invocations is
not guaranteed to reproduce the original execution, just as in the Python path.
Private outputs preserve application storage; they do not make atomic ordering
deterministic.

## Export, UI and projects

`geometry.json` retains the reference metadata, attempts and SHA-256;
`vertices.bin` stores the actual packed shader outputs; `vertices.csv` stores
typed values. `geometry.obj` uses complete finite float4 SV_Position divided by W
and point/line/triangle primitives. Missing position, nonfinite values or zero W
retain raw attributes, report why OBJ is unavailable and remove stale OBJ files.
The worker's additional `geometry-ui.json` feeds the Qt table and mesh.

The mesh and OBJ share one-based primitive indices; CSV vertex identifiers remain
zero-based. A rendered triangle regression catches the earlier index mismatch
that displayed only one edge. The preview retains the existing 12,000-edge/point
display cap; exports retain the complete captured geometry. Geometry controls
clear stale results and exports when changed. UI copy is limited to controls,
counts and tooltips.

Experiment projects save/restore migrated stage, stream, instance and IA table
settings using the Python `geometry_selection` field names and legacy values.
An optional `flora_geometry_stage` field preserves the native UI's IA selection.
Other UI/project fields are retained. Instance values must fit uint32 for native
draw calls. Unmigrated stage selections are not available in the current UI.

```powershell
.\out\FloraGPA-post-transform\FloraGPA.Cli.exe post-geometry D:\captures\sample.gpa_frame --event 100 --geometry-stage final --stream 0 --out D:\results\geometry
# Add --instance 2 for the third original instance, or --warp for software replay.
```

## Validation and remaining scope

Development validation compares exact report fields and raw output bytes, parsed
typed CSV values and OBJ geometry with Python. Positive cases must succeed;
matching failures do not count as successful capture coverage. The final package
checks and regression results are recorded in the migration status document.

The final Release package passes 140 synthetic comparisons (130 successful
captures and 10 expected rejections) and 14 original GF2/BF1 comparisons on
Hardware/WARP, including BF1 tessellation at Final/VS/DS. GF2/BF1 full-frame golden
images and suppressed-draw negative controls pass. Release CTest passes all 39
suites; the existing UI suite passes all 53 cases and the new geometry suite
passes all eight cases, including setup/cleanup. The first UI regression run
failed two outdated button-text lookups; they now use the stable action ID and
retain their original IA data assertions.

Evidence is in `artifacts/post-transform-portable-final/validation.json`,
`artifacts/post-transform-real-portable-final/validation.json`,
`artifacts/post-transform-golden-final/validation.json`,
`artifacts/ctest-post-transform-final.log` and
`artifacts/post-transform-tests-final.txt`. The final screenshot is
`artifacts/post-transform-ui-final/post-transform.png`.
The runtime audit of 148 successful reports finds no Python/Tk/GPA/RenderDoc
modules and verifies package-local Qt and identical build/package EXE bytes
(`artifacts/post-transform-runtime-audit.json`). These local captures, binaries
and evidence remain outside Git.

`PostTransformTests` covers original UAV bytes/counters and render-target storage,
normal and dirty SO cursors, signature-only providers, native continuation after
inspection, overflow followed by a memory-limit failure and a successful retry,
disabled draws and actual zero-W shader output. Qt tests exercise the real Worker,
rendered mesh, result invalidation, file export and project save/restore.

This batch does **not** complete all geometry or advanced analysis features:

- `vs-index`, `vs-writes`, `ds-writes`, `hs` and `gs-emits` remain separate pending
  migrations. The CLI rejects those stage requests instead of substituting output.
- Quad/coverage consumers and general private-output modes that copy original SO
  buffers or relocate UAV slots remain pending. This helper implements the
  retain-original-SO path used by native post-transform capture.
- RenderDoc-backed mesh/debugging, pixel history and shader stepping are not
  implemented by this batch.
- Final-stage instance/stream persistence is implemented; unsupported geometry
  stage project restoration must be completed with those stage migrations.
- Large captures still materialize attribute tables and mesh JSON in memory.
  The SO byte limit is not a bound on total process memory. Large-output stress,
  broader dynamic class-linkage and live-predicate isolation combinations, and
  a separate clean-machine run remain verification gaps.

The module manifest remains partial and the full Python-to-C++ migration remains
open. This evidence establishes the listed paths on this host, not universal
coverage of arbitrary captures or drivers.
