# Output binding migration

The native `OutputBindingModel` ports the original/experimental state machine in
`standalone/binding_model.py`. Output/SO argument validation and cached history
now connect to experiment projects, native replay and the compact Qt setter
editor. Native SO cursors, extended UAV slots, and final-binding consumers are
integrated. Full parity still depends on pending global view, texture experiment
and private diagnostic modules.

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
- Replay rebuilds history per run, executes edited payloads at their original
  command boundary and applies deltas before event edits. Retained SO slots use
  native append only when their cursor is known. Original later setters and
  ClearState still replace bindings. Native output getters verify accepted state;
  skipped missing-original setters are not falsely treated as executed.
- Graphics snapshots preserve extended CS/OM UAV slots. Buffer patch validation,
  counter edits/inspection, SRV descriptors and geometry resolve final bindings.
  Counter and buffer edits validate after all project operations, independent of
  operation order.
- Qt **Edit Setter** supports RTV/DSV, OM/CS UAVs and all five SO setter families,
  with independent optional arrays, KEEP groups and uint32 counter/offset cells.
  Unchanged forms do not add history; invalid edits remain in the dialog.

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

The subsequent replay batch passed 1,142 initial comparisons in
`artifacts/output-replay-full-01/validation.json`. An additional 132 comparisons in
`artifacts/output-replay-extended-02/validation.json` cover hidden UAV counters,
buffer/counter composition in both orders, both KEEP forms, slot 63, six-stage
collateral changes, mixed SRV edits and edited GF2/BF1 boundaries on hardware/WARP.
These are intermediate evidence. The legacy synthetic counter fixture is copied with explicit
immediate context 1 in CopyStructureCount records; original source files are intact.

The final isolated package `out/FloraGPA-output-editors/` passes **1,992 checks**
in `artifacts/output-replay-package-full/validation.json`, including all five SO
layouts, event boundaries, disabled events, output bytes, all 1,008 binding fields,
hidden counter metadata, known/unknown retained SO cases, final IA geometry and
both real captures. Validation retains null-array distinctions and compares
resource identity groups without comparing process-specific pointer addresses.
GF2/BF1 full-frame hashes and suppressed-draw negatives pass in
`artifacts/validation-output-editors-golden/validation.json`.

`artifacts/output-editors-audit.json` records 1,242 successful native reports,
75 loaded module paths and no Python, GPA or RenderDoc modules. The three packaged
executables match the corresponding Release build hashes. This is host-local
isolation evidence, not validation on a separate clean Windows installation.

Release and Debug `OutputBindingTests` each pass all nine tests, including saved
project reload, rejected edits preserving history, undo/redo, counter/buffer
composition and repeated replay on the same replay object. The three new Qt tests
pass in both configurations: parameter validation, SO offsets, and MainWindow
menu/Worker/undo/redo integration. Logs are `ctest-output-bindings-final-*.log`
and `output-ui-final-*.txt` under `artifacts/`; screenshots in
`artifacts/output-ui-final-release/` were visually reviewed.
The final Release CTest run passes all 25 suites, including all 35 Qt UI tests,
in `artifacts/ctest-output-editors-final-release.log`.

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

Replay State continues to report actual native getter values. The offline model
retains the Python semantics. No synthetic zeros are substituted for native null
IA stride/offset observations. Output getter verification checks outputs, not
unobservable SO cursors or canonicalized IA metadata; the documented discrepancy
remains a limitation of offline-to-native equivalence.

## Remaining integration

1. Port global view and texture input/output experiments, including private
   clones, and resolve their consumers against final output bindings.
2. Close coverage, quad, post-shader and other private diagnostic consumers.
3. Add remaining setter families to the shared history as they are migrated.
4. Keep full output module parity partial until every reference consumer and
   supported combination has native execution evidence.
