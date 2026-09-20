# Output binding migration

The native `OutputBindingModel` ports the original/experimental state machine in
`standalone/binding_model.py`. This is core infrastructure for the remaining
output setter editors. Output/SO argument validation and cached history are now
implemented. Experiment project integration, native replay integration,
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
- `application/OutputEdits` checks complete argument objects, immediate contexts,
  unsigned integers, independent null/empty arrays, RTV/UAV view types and target
  dimensions. It preserves the original 16-byte command prefix when encoding.
  All five SO setter families validate unique non-null buffers, bind flags and
  aligned/in-bounds offsets; omitted offsets remain omitted, not zeroed.
- `OutputBindingHistory` owns immutable replacement payloads, advances each
  context once and caches only nonempty deltas. Earlier reads do not replay the
  model; a malformed future record does not affect an earlier requested event.
  Missing commands that could reset a missing/counter UAV reject explicitly.
- State overlays mark dirty non-null SO slots as retained and set their offset
  to KEEP. This is a request for append preservation; it does not prove that the
  native hidden cursor is known.

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

The subsequent argument/history batch is covered by
`tools/validate_output_history_port.py`: 65 groups and 2,480 comparisons, with
548 argument/encoding cases and 1,932 history/state reads. This includes 1,990
accepted operations and 490 expected rejections. Evidence is in
`artifacts/output-history-combined/validation.json` for Release and
`artifacts/output-history-debug/validation.json` for Debug. Coverage includes
all five SO encodings, six-stage SRV collateral changes and mixed edits, IA,
interleaved contexts, reverse cache reads, future malformed records, counter
gaps and GF2/BF1 snapshots. The earlier 8,152-boundary model/native-getter
regression is rerun in `artifacts/output-history-model-regression/validation.json`.

These APIs are deliberately separate from `isEditableSetter` until replay and
project consumers are connected. The packaged UI still exposes the previously
completed predicate, sampler and SRV setters.

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

1. Connect validated output/SO payloads to experiment transactions, save/load and
   undo/redo. Recreate the immutable history whenever the experiment changes;
   include modeled SRV/IA replacements in the same history.
2. Pass cached deltas and gap evidence through the worker/replay boundary rather
   than independently rebuilding or guessing the affected output state.
3. Apply deltas before event edits and resource cloning, including final target
   resolution for buffer/texture edits and export consumers.
4. Execute replacements at their command boundary, retain edited SO cursors and
   UAV counters across snapshots, and check native outputs after void setters.
5. Connect the compact Qt setter form, worker requests, project save/load and
   undo/redo; verify selection/revision cancellation and all reference consumers.
6. Close global view edit, private diagnostic and other pending dependencies
   before marking full output editing or the overall migration complete.
