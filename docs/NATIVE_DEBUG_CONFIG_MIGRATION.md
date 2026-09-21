# Native debugger configuration and Qt controls

`NativeDebugConfig` ports `native_debug_config.py`. `NativeDebugControls` ports
the breakpoint/watch controls to a compact Qt Widgets component. The component
is now integrated into the main-window GS / HS / DS checkpoint views, including
native Worker capture and original source-frame readers. Integration evidence
and the current package are recorded in
[CHECKPOINT_UI_MIGRATION.md](CHECKPOINT_UI_MIGRATION.md).

## Configuration compatibility

- GS, DS and HS retain their original versioned format names. Shader SHA-256 and
  canonical hashes of catalog, source lines, variable symbols and source stacks
  bind settings to the original stage and mappings. Mapping hashes use Python's
  sorted compact UTF-8 JSON rules, including float notation and Unicode keys.
- A complete replacement is validated before live state changes. Unknown fields,
  wrong identities, duplicate or unmapped breakpoints, malformed expressions and
  invalid hit rules fail without replacing existing rules or watches.
- Limits remain 256 source breakpoints, 4,096 instruction breakpoints, 64 watches
  and 1 MiB for configuration files. Conditions and watches use the native
  read-only expression parser. Instruction breakpoints retain original IDs.
- Reading supports UTF-8 and detected UTF-16/32, with or without BOM, as Python's
  byte-based JSON reader does. Saving writes UTF-8 through `QSaveFile`; size and
  serialization are checked before replacing the destination. JSON parsing and
  mapping hashing additionally reject nesting beyond 128 levels. Valid native
  configuration schemas do not require such nesting.

The Qt component exposes source-line selection, conditional/count rules,
watch add/remove, selected-record/frame values, and configuration import/save.
Unavailable values remain explicit; individual expression errors do not discard
other watches. SDBG results retain the `sdbg_assignment_history` basis and refuse
single-point or unverified ancestor-frame evaluation. The parent checkpoint
view supplies native register/value readers and source selection signals.

## Validation

- `artifacts/native-debug-config-final/validation.json`: 984 differential checks
  passed, with 720 exact successful results and 264 consistent rejections.
  Includes identities from all 164 existing native capture reports (19 distinct
  original shaders), configuration round trips, mapping mismatch rejection,
  cardinality/type/duplicate limits, normalized expressions, random finite
  double hashes, Unicode/control characters, eight file encodings and file-size
  boundaries. No GPU recapture was needed for these read-only checks.
- `artifacts/ctest-native-debug-config-final.log`: checkpoint model suite passed.
  The native configuration test verifies failed-import rollback, source change
  rejection, strict integer rules, Unicode paths, preservation of a saved file
  after an oversized write and excessive-nesting rejection.
- `artifacts/debug-controls-ui-final.log`: the new Qt panel test passed. It
  exercises rule creation/selection/removal, watch values, duplicate rejection,
  missing variables, record/frame changes, retained settings across refresh,
  import rollback, real Qt file-dialog save/import, SDBG history basis and
  unsupported ancestor/single-point states. Existing full-window tests were not
  relabeled as new evidence for an unwired component.
- `artifacts/debug-controls-ui-final/native-debug-controls.png`: actual Qt
  rendering inspected at 680×470; dark compact tables and toolbars, without
  explanatory paragraphs. Final checkpoint-layout integration needs its own QA.
- `artifacts/build-debug-config-final.log`: full Release build passed without
  compiler warnings. Replay behavior and the delivered package were unchanged;
  no new golden-frame or runtime-package claim is made for this component batch.

Reproduce the development-only configuration checks:

```powershell
python tools/validate_native_debug_config.py --reference D:/CDXrepo/FloraGPA --exe build/vs2022/Release/FloraCheckpointTests.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --captures artifacts/sdbg-capture-final --out artifacts/debug-config-check
```

Use a fresh output directory. Python is only the development oracle, never part
of the application runtime. The original component-only evidence above is
supplemented by the main-window and capture integration checks documented in
[CHECKPOINT_UI_MIGRATION.md](CHECKPOINT_UI_MIGRATION.md).
