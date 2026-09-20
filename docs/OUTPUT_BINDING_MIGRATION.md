# Output binding migration

The native `OutputBindingModel` ports the original/experimental state machine in
`standalone/binding_model.py`. This is core infrastructure for the remaining
output setter editors. Output/SO experiment serialization, replay integration,
retained SO cursors and the Qt editor are **not yet available**.

## Implemented

- Independent original and changed states, including unknown values, protected
  writes and uncertain collateral changes across snapshots.
- 1,008 fields: six-stage SRVs, vertex buffers/strides/offsets, index buffer/format/
  offset, eight RTVs, 64 OM UAVs, 64 CS UAVs, four SO targets and DSV.
- RTV/DSV size, layer count, sample count/quality and resource dimension checks;
  output/output conflicts; SRV and IA collateral unbinding.
- KEEP-RTV, KEEP-UAV, range movement, explicit null arrays versus absent arrays,
  ClearState and snapshot overlay, including extended output slots.
- Missing output records invalidate only possibly affected bindings. Edited
  unknowns cannot be silently replaced by a later captured snapshot. Invalid
  edits leave both histories unchanged.

`readOutputCommand` is shared by actual captured command replay and SRV history.
The experimental model uses whole-subresource overlap, matching Python;
read-only captured-state inspection retains its existing uncertainty rules.

## Verification

`tools/validate_output_model_port.py` drives a development-only native test
executable. At every boundary it compares canonical hashes of **all** original
and changed fields, dirty field names and the overlay round trip. It does not
use the Python backend in the application.

The final comparison covered 42 streams and 8,152 boundaries:
synthetic edited streams, repeated original snapshots, missing views and
recovery, invalid edits, reproducible mixed command sequences, and the original
GF2/BF1 captures. It also compares 288 synthetic boundaries with native getters
on hardware and WARP. Evidence is in
`artifacts/output-model-verified/validation.json`; tested source and executable
hashes are in `artifacts/output-model-build-hashes.json`.

The package `out/FloraGPA-output-binding-model/` passes the GF2/BF1 golden images
and both suppressed-draw negative checks with a Windows-only runtime PATH.
Release CTest passes all 25 suites; Debug passes output, input and SRV binding
suites. These checks verify the shared parser integration without claiming that
the pending output setter editor has been implemented.
The packaged CLI also passes all 164 existing SRV edge comparisons in
`artifacts/output-model-srv-edges/validation.json`, including output gaps,
descriptor/buffer inheritance, immediate-context order and read-only DSV planes.

### Native ClearState discrepancy

The Python model sets all IA stride/offset fields to zero after ClearState.
On this machine both hardware and WARP preserve the stride/offset of a vertex
slot that was **already null because its buffer conflicted with an output**.
The buffer binding itself is null in both results. A sequence that reproduces
this is ClearState, CS UAV for a buffer, IA vertex binding of that same buffer
with stride 4/offset 12, UAV unbind, ClearState, then IAGetVertexBuffers.

The oracle records these differences explicitly as `native_discrepancies` and
sets `native_exact` to false for the affected stream. It does not discard these
fields or report exact native parity. Original/changed model comparisons still
include them and match Python. Two ClearState boundaries and their subsequent
snapshot anchors differ in the current hardware/WARP tests (four observations).

Before exposing output edits, decide how replay establishes canonical null IA
metadata after ClearState, and verify repeated replay and actual getter output.
Do not silently manufacture a runtime observation from the offline model.

## Remaining integration

1. Validate and serialize output/SO setter arguments transactionally, including
   independent optional arrays, unique SO resources and exact buffer offsets.
2. Cache per-context history deltas and missing-command evidence; reject skipped
   output commands whose UAV counter initialization cannot be reconstructed.
3. Apply deltas before event edits and resource cloning, including final target
   resolution for buffer/texture edits and export consumers.
4. Execute replacements at their command boundary, retain edited SO cursors and
   UAV counters across snapshots, and check native outputs after void setters.
5. Connect the compact Qt setter form, worker requests, project save/load and
   undo/redo; verify selection/revision cancellation and all reference consumers.
6. Close global view edit, private diagnostic and other pending dependencies
   before marking full output editing or the overall migration complete.
