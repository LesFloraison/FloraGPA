# Native GS / HS / DS checkpoint workspace

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

The main window now hosts three independent checkpoint views under **Shader
Debug**, backed by the native `shader-checkpoint` Worker command. This ports the
recovered `gs_checkpoint_ui.py` workflow to Qt Widgets and connects the previously
ported source navigation, symbols, value histories and configuration controls.
It does not complete the entire Python application migration; pixel history and
the separate pixel/vertex/compute debugging workflows remain pending.

## Workflow and layout

Select a draw in **API Log**, open **Shader Debug**, and choose GS, HS or DS.
**Read** loads its original instruction catalog. **Capture** records values before
the chosen original instruction; **Trace** records instruction execution, and
**Trace Input** re-executes using the selected record's exact declared input bits.
Unique/all matching retains the original selector semantics. HS traces use the
selected instruction's original phase; selected-input traces use the record's
phase. Invocation numbers do not identify executions across separate captures.

The existing GPA-inspired main layout remains: chart above, API/resources left,
analysis in the center and properties/metrics right. The debugger has compact
toolbars, assembly/source tabs on the left, snapshots above registers/variables/
breakpoint-watch tabs on the right, and a one-line stack footer. Limits and full
record/value metadata are available through tooltips. Splitters resize the views;
long source lines and tables scroll instead of expanding the application.

- Instruction/source navigation provides previous, step into, step over, step
  out and continue. Run to uses an original instruction ID in either mode.
- Source breakpoints retain conditions and hit-count rules. Configuration import
  and save use the existing shader/mapping-bound native format and atomic writes.
- Registers show hex, uint, int or float with explicit component validity.
  Unwritten components remain blank. Source variables include SPDB locals and
  SDBG assignment histories; unsupported or unavailable values stay explicit.
- Source-frame selection shows the corresponding original location, owned locals
  and frame-scoped watches. Stepping always operates on the execution position.
- Each stage retains its own output and configuration. A capture refresh retains
  compatible settings; Read resets them as the reference controller does.
- Event, experiment document, capture identity and driver form the current
  context. Changes leave old records inspectable but require Read before further
  capture. Existing results can still be exported when a non-draw is selected.
- Native Worker revision checks discard cancelled or superseded results. Output
  directories stay alive for the selected view, including after other jobs run.

## Export and bounds

Export creates a native ZIP containing enriched `checkpoint.json`, original
`shader.dxbc`/`shader.asm`, complete `snapshots.bin`, `hits.csv`, `registers.csv`,
runtime `result.json`/`loaded_modules.json`, and an input selector when present.
Selected variables, source frame, watch results and debugger settings use the
reference schema. The runtime report comes from FloraGPA's actual Worker.

The writer streams file members in 1 MiB blocks, computes CRC32 and replaces the
destination atomically with `QSaveFile`. Unsafe/duplicate names and missing or
truncated source files fail without replacing an existing archive. ZIP members
use STORE rather than the reference's DEFLATE, so archives are larger but their
contents remain directly readable with standard ZIP tools. ZIP64 encoding is
implemented; archives exceeding 4 GiB have not been exercised in this batch.

The UI retains the reference's 10,000-row preview. Exports retain full binary/CSV
files. Artifact reads are bounded at 256 MiB; navigation never invents records
beyond the visible invocation or HS phase. Symbol/stack and legacy-history limits
from the native parsers remain in force. This does not extend the Python debugger
to unsupported shader formats or reconstruct missing captured data.

## Validation

- `artifacts/build-checkpoint-ui-shipping.log`: full Release build passed without
  compiler warnings. Package: `out/FloraGPA-checkpoint-ui/FloraGPA.exe`.
- `artifacts/checkpoint-ui-shipping.log`: 10 Qt cases passed with no skips. This
  includes six GS/HS/DS Hardware/WARP main-window cases, real Worker catalog /
  point / trace / input recapture, HS phase changes, cancellation/retry, event /
  experiment / undo / driver invalidation and archive replacement failure tests.
- `artifacts/checkpoint-ui-shipping/validation.json`: 3,246 reference checks
  passed across 44 real trace sessions. Actual selected values, ancestor-frame
  locals/locations and watch results match Python. There are 1,752 actual Qt
  navigation-button outcomes, including successful/rejected instruction/source
  step, over, out, run-to and conditional/count continue operations. This includes
  12 successful source-function outs and two original assembly-call outs.
  Eight checkpoint ZIPs plus the Unicode/empty/binary transaction ZIP pass an
  independent Python `zipfile` CRC/content/schema check.
- Actual GS/HS/DS main-window captures and SPDB/SDBG source/variable/watch views
  in `artifacts/checkpoint-ui-shipping/` were rendered and visually inspected.
  The SDBG history tab and scope tooltip distinguish observed assignment values.
- `artifacts/ctest-checkpoint-ui-final.log`: checkpoint model, post transform,
  shader, core and existing UI suites passed. The existing UI suite reports
  54 passed, zero failed/skipped. Subsequent UI-only changes were column widths,
  English SDBG scope labels and history/scope hints, covered again by the final
  checkpoint UI run; production replay code did not change afterward.
- `artifacts/checkpoint-ui-capture/validation.json`: 164 packaged native/Python
  source-capture comparisons passed, including complete source metadata and
  per-invocation register histories. The historical `pending` field in this
  capture-only report describes areas outside that validator; the generator now
  names it `outside_scope`. UI/configuration evidence is listed separately here.
- `artifacts/checkpoint-ui-golden/validation.json`: GF2/BF1 images and their
  suppressed-draw negative controls passed (four cases). The tested CLI is
  byte-identical to the delivered CLI; later changes affected only Qt labels.
- `artifacts/checkpoint-ui-runtime-audit.json`: all 166 packaged capture reports
  (164 source captures and two raw-call traces) passed module checks. Qt comes
  from the package and the system disassembler is loaded from System32; no
  Python/Tk/GPA/RenderDoc/DIA runtime was loaded. All three packaged executable
  hashes match the final Release build. This is host-local isolation evidence,
  not a claim of testing on another clean Windows machine.
- `artifacts/checkpoint-ui-package.log`: all six main-window Hardware/WARP
  capture cases also passed against the deployed Worker and Qt with Windows-only
  PATH (eight Qt results including initialization/cleanup). A temporary native
  test harness and Qt Test DLL were removed afterward. Its six exported runtime
  reports independently confirm the package Worker/Qt paths in
  `artifacts/checkpoint-ui-package/runtime-validation.json`.

Reproduce the UI/reference comparison using external captures generated by
`tools/validate_checkpoint_capture.py --source-only`:

```powershell
# Run from the repository root; set external paths for your environment.
$QtRoot = 'C:/Qt/6.11.2/msvc2022_64'
$ReferenceRoot = 'C:/reference/FloraGPA'
$ResultsRoot = Join-Path (Get-Location) 'artifacts/results'
$env:PATH = "$QtRoot/bin;" + $env:PATH
$env:QT_QPA_PLATFORM = 'offscreen'
$env:FLORA_UI_ARTIFACT_DIR = "$ResultsRoot/checkpoint-ui"
$env:FLORA_CHECKPOINT_FIXTURES = "$ResultsRoot/source-captures"
.\build\vs2022\Release\FloraCheckpointUiTests.exe
python tools/validate_checkpoint_ui.py --reference "$ReferenceRoot" --artifacts "$ResultsRoot/checkpoint-ui"
```

Use a fresh artifact directory. Multiple fixture roots can be separated by `;`.
The external fixture test explicitly skips without this variable; the six native
Hardware/WARP main-window capture cases generate their own fixtures. Python/Tk
are used only by the development oracle, not by FloraGPA or its Worker.
