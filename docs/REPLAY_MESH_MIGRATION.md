# Native post-shader replay mesh

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

> **Subsequent work:** [Scheduled Intel collection](MD_ITERATIONS_UI_MIGRATION.md) and [uniform Intel UI](UNIFORM_METRICS_UI_MIGRATION.md) provide later hardware-metric integration. They do not close the mesh or clean-machine limits below.

The optional RenderDoc **postmesh** analysis is now implemented in C++ and
connected to **Geometry → Replay Mesh**. It preserves the original Python
advanced panel's VS output / final GS-or-DS output and instance selection,
rotatable mesh preview, and JSON / CSV / OBJ export. The existing independent
geometry inspector remains available in the adjacent **Independent** tab.
The desktop and worker do not invoke Python, Tk or Intel GPA.

## Interface and preserved semantics

`FloraGPA.Rdc.exe --job job.json` accepts:

```json
{
  "action": "postmesh",
  "capture": "D:/captures/independent_capture.rdc",
  "renderdoc": "C:/Program Files/RenderDoc/renderdoc.dll",
  "out": "D:/results/mesh",
  "gpa_event": 105,
  "stage": "VSOut",
  "instance": 0
}
```

`stage` is `VSOut` or `GSOut`; the latter returns the final GS/DS output as in
the original worker. Select an executed draw; dispatches, disabled commands
and stages without mesh data fail explicitly. Instance values are checked
uint32. The optional backend remains the selected RenderDoc 1.45 library.

The result retains event/command maps, the fourteen original mesh metadata
fields and vertex/index/face counts. Artifacts are `post_vertices.bin`,
`post_indices.bin` when indexed, `post_vertices.csv`, and `post_geometry.obj`.
Position decoding preserves captured stride, component count, index width,
signed base vertex and nonindexed behavior. Binary output precedes float-format
validation, matching the original branch. CSV and OBJ use the original Windows
line endings and Python-compatible float representation, including signed zero
and nonfinite values. OBJ divides XYZ by W when W is available and nonzero.

Triangle lists retain degenerate faces. Triangle strips alternate winding and
discard degenerate triangles. Out-of-range faces are omitted from OBJ, while
the public `face_count` remains the original **candidate** face count before
that filtering. Primitive-restart values receive the original ordinary-index
treatment; no new strip restart interpretation is introduced. Other topologies
export positions without synthesizing faces.

The Qt view shows raw position components in a virtual table and projected
positions in the shared wireframe viewer. Unrenderable positions retain their
raw values and exports. Details are collapsed by default. Loaded artifacts are
owned by the view, so exports survive the next worker's temporary-directory
cleanup. Frame, experiment, adapter, stage, instance and backend changes
invalidate stale results. Cancellation and capture caching share the existing
Pixel History / Shader Debug / Replay Metrics pipeline.

## Validation

Development evidence:

- `artifacts/rdc-mesh-parity/validation.json`: ten actual native/Python replay
  comparisons, four rejected requests, 3,652 vertices, 19,416 indices and
  713,173 exact artifact bytes. Coverage includes source VS on hardware/WARP,
  16-/32-bit indexed draws, GF2, BF1, and GS/DS on hardware/WARP.
- `artifacts/rdc-mesh-instances/validation.json`: eight additional VS/GS
  comparisons for instances zero and one on hardware/WARP, plus one repeated
  source baseline. All metadata and export bytes match the original worker.
- `artifacts/replay-mesh-model-oracle/validation.json`: 78 constructed cases
  execute the **actual original postmesh AST branch**, with only buffer
  transport mocked. Coverage includes one-/two-/four-byte indices, base
  vertex, strip winding/degenerates, invalid references, component counts,
  trailing bytes, truncated storage, zero stride, unsupported formats,
  signed zero, infinity, NaN and deterministic random float bit patterns.
- `tests/ReplayMeshUiTests.cpp`: recorded reports, complete table/export
  comparison, retained artifacts, invalid-result rollback, stale tokens,
  uint32 instance validation, real hardware/WARP recapture, debug cache
  sharing, cancellation/retry, missing stages/backend, experiment disable/undo
  and the existing independent Geometry entry point.

Reproduce the model oracle using `tools/validate_replay_mesh_model.py` and the
real controller comparisons using `tools/validate_rdc_mesh.py`. These Python
scripts and qrenderdoc are development oracles only. Set `FLORA_MESH_REPORTS`
to the comparison output and `FLORA_DEBUG_SOURCE_CAPTURE` to the original
`shader_sources/source.gpa_frame` for fixture-dependent Qt tests.

## Delivery checks

Package: `out/FloraGPA-replay-mesh`. Its full native/Python controller matrix
passed **18 comparisons and four rejected selections**, covering **3,676
vertices, 19,432 indices and 715,705 exact artifact bytes**. See
`artifacts/rdc-mesh-package/validation.json`.

The complete Release build passed. Six other relevant CTest suites passed,
including all **54 original main-window cases**, with the new mesh suite
subsequently passing **six packaged Qt cases, without skips**, under a
Windows-only PATH. The actual packaged hardware/WARP layouts were inspected.
The first regression run's new-suite failures were test assumptions: a
procedural SV_VertexID draw has no IA layout, and a repeated analysis has a
different temporary output path and DLL enumeration order. The corrected
test explicitly selects independent VS output and compares the current
report before/after switching inspectors. Original failure evidence remains
in `artifacts/replay-mesh-ui-initial-failure.txt` and
`artifacts/replay-mesh-shipping-initial-failure.txt`; report differences are
retained under `artifacts/replay-mesh-shipping-final/replay-mesh`.

UI evidence: `artifacts/replay-mesh-shipping-final.txt` and
`artifacts/ctest-replay-mesh-regression.log`. The latter records the initial
new-suite failure and the passing existing suites, not an entirely green run.

All **four GF2/BF1 golden-frame and suppressed-draw controls** passed through
the packaged CLI (`artifacts/replay-mesh-golden/validation.json`). Auditing
**34 runtime reports** found no Python/Tk/GPA modules; Qt loaded from the
package, and RenderDoc appeared only in explicit capture/analysis jobs.
All four product executable hashes match Release. The temporary Qt test
executable and Qt Test DLL were removed from the delivery directory. See
`artifacts/replay-mesh-runtime-audit.json`. These are isolated-path checks on
this host, not a separate clean-Windows-machine validation.

## Remaining migration

This completes the implemented postmesh path and its desktop connection on
the tested corpus, not the entire Python migration. The preview uses the
existing mesh renderer's bounded wireframe sampling; raw values and exports
remain complete. Hardware-specific metric scheduling, GTPin, other pending
consumers and broader source-module audits remain tracked separately.
Shared modules remain `partial` pending those audits and outstanding work.
