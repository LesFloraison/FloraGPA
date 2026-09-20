# Native checkpoint capture and export

The C++ application layer now captures original GS/DS/HS instructions through
`inspectCheckpoint` and the `shader-checkpoint` CLI command. This builds on the
verified native DXBC transform. Source variables/stacks and Qt checkpoint
views are still pending; this is not the complete Python debugger migration.

## Available behavior

- Catalog original instruction numbers and disassembly without submitting a
  diagnostic draw; select a single instruction or a whole invocation trace.
- Replay to the original before-event boundary, apply experiment bindings and
  scoped inputs, and submit original direct or indirect Draw parameters.
- Preserve downstream shaders, class instances and original GS stream-output
  declarations. Reserve a free graphics UAV while accounting for bound views
  and declarations in all active graphics stages.
- Capture through private RTV/DSV/UAV/SO storage, retain hidden UAV counters and
  tracked SO byte cursors, and restore original resources/bindings on success
  and failure. Diagnostic Draws do not change replay command accounting.
- Initialize HS runtime token, capacity and exact input-filter values for each
  original control-point/fork/join phase. Trace requires an explicit HS phase.
- Grow capacity with at most three attempts and a default 256 MiB byte limit.
  Reject counter wrap, executed invalid array accesses and HS output-address
  faults. Completion and pipeline-statistics reads are bounded waits.
- Match complete original input bits using `all` or `unique`; reject no matches
  or multiple matches under `unique`. Independently verify returned input bits
  and the number of complete matched invocation histories.
- Decode ordered hits, original static call stacks, register values and
  per-component validity. Reject malformed record layout, headers, hit sequences,
  call depth and validity flags. Preserve raw NaN payloads and signed zeros.
- Export `checkpoint.json`, original `shader.dxbc` and `shader.asm`, and (when
  captured) `snapshots.bin`, `hits.csv` and `registers.csv`. JSON previews stop at
  10,000 hits; binary and CSV exports retain all records.

## Command line

Use a new or empty output directory for each operation. Instruction numbers
refer to the original disassembly, not DXBC token indices.

```powershell
# List original instructions.
.\FloraGPA.Cli.exe shader-checkpoint frame.gpa_frame --event 100 --shader-stage gs --out catalog
# Capture immediately before one original instruction.
.\FloraGPA.Cli.exe shader-checkpoint frame.gpa_frame --event 100 --shader-stage gs --instruction 7 --out point
# Capture a complete selected HS phase.
.\FloraGPA.Cli.exe shader-checkpoint frame.gpa_frame --event 100 --shader-stage hs --trace --hs-phase 0 --out trace
# Select every invocation whose declared inputs match the selector's raw bits.
.\FloraGPA.Cli.exe shader-checkpoint frame.gpa_frame --event 100 --shader-stage ds --trace --input-selector inputs.json --out selected
```

`--warp`, `--experiment`, `--disable` and `--suppress-draws` use the existing
replay behavior. `--max-bytes` bounds the diagnostic allocation. Selecting an
output stream or draw instance is not part of the Python checkpoint operation;
these options are rejected. A disabled draw exports zero snapshots without
inventing hits. Selectors remain tied to the exact original shader and HS phase.

## Evidence and limits

`tools/validate_checkpoint_capture.py` is a development-only Python oracle.
`artifacts/checkpoint-capture-parity-v3/validation.json` records 540 passing
comparisons: 324 successful operations (26 catalogs and 298 captures), plus 216
matching rejections. The 298 captures also have identical raw snapshot bytes.
Coverage includes Hardware/WARP, GS 4.0/4.1/5.0 indexable temporaries, original
CALL/CALLC, active GS SO, tri/quad/isoline DS and HS, HS join phases, culled DS,
single instructions, traces and selectors. The comparison retains each complete
invocation's ordered history and independently checks exported CSV contents.
Global atomic allocation order may differ in other captures and is recorded
separately, without treating invocation IDs as stable identities.

The final package is `out/FloraGPA-checkpoint-capture`. Its CLI has 60 additional
passing comparisons in `artifacts/checkpoint-capture-integration-v1/validation.json`:
indexed/nonindexed indirect draws for GS/HS/DS, original GPU-populated arguments,
scoped vertex-input edits, scoped indirect-argument edits, disabled events and
suppressed draws, each on Hardware/WARP. The package retains the current desktop
UI; the new checkpoint operation is currently exposed through the CLI only.

Final validation also includes:

- Release build: `artifacts/build-checkpoint-capture-final.log`.
- All 41 CTest suites passed, including 53 existing Qt UI cases without skips:
  `artifacts/ctest-checkpoint-capture-final.log`.
- Existing VS/DS write and GS emission exports: 100 passing comparisons including
  real GF2/BF1 cases, in `artifacts/checkpoint-capture-log-regression/validation.json`.
- GF2/BF1 golden replay and draw-suppression controls: four passing checks in
  `artifacts/checkpoint-capture-golden/validation.json`.
- `artifacts/checkpoint-capture-runtime-audit.json`: 154 successful package
  reports without Python/Tk/GPA/RenderDoc modules, Qt DLLs loaded from the package,
  and all three packaged executable hashes matching the Release build. This is
  host-local isolated-path validation, not a separate clean-machine test.

Native `PostTransformTests::checkpointCaptureAndIsolation` exercises all three
stages and every fixture HS phase on Hardware/WARP. It checks successful capture,
capacity rejection, input filtering, original resource bytes/counters, unchanged
accounting, subsequent Draw results, and disabled/suppressed draws. A short HS
phase legitimately fits the initial allocation; the initial test's requirement
that every phase must retry was corrected using observed capacities and counts.
The original failure is retained in `artifacts/checkpoint-capture-native-v1.txt`.

`CheckpointTests::decodedRecordsAndExports` checks malformed data rejection,
call-stack transitions, raw NaN payloads, infinities, signed zero, and blank CSV
values for unwritten components. Its initial focused result is in
`artifacts/checkpoint-decoder-v1.txt`.

The subsequent [source-line batch](SOURCE_LINES_MIGRATION.md) adds `source_lines`
and catalog source locations; the parity tool now compares them. Reports use
`source_debug_status: source_variables_and_stack_pending`. Python
`source_variables` and `source_stack` remain excluded from parity and unimplemented.
Qt checkpoint controls, source stepping, locals, watches and source breakpoints
remain part of the full migration goal.

These synthetic capture comparisons do not establish arbitrary-shader coverage.
The previous transform matrix includes real GF2/BF1 shader bytecode, but that is
not evidence of complete production checkpoint captures for those games.
