# Qt Pixel History

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

> **Subsequent work:** Later batches connect [recorded debugging](REPLAY_DEBUG_UI_MIGRATION.md), [Replay Mesh](REPLAY_MESH_MIGRATION.md) and [Replay Metrics](REPLAY_METRICS_MIGRATION.md). The pending consumers below describe this earlier batch; its specific fault-injection and clean-machine gaps are not silently closed.

The main-window **Pixel History** tab now drives independent GPA replay recapture
and the native RenderDoc history worker. It retains the existing GPA-style event
chart, API/resource browser and inspector layout. The history view uses compact
controls and a table; full JSON details are collapsed by default.

## Workflow

- Select an API event, open Pixel History, set a resource (blank means target 0),
  pixel, mip, layer/volume slice and MSAA sample, then **Read**. The API field can
  also select a non-draw command such as UpdateSubresource.
- **RenderDoc…** chooses the optional installed 1.45 DLL. The choice persists in
  application settings. No Python/qrenderdoc is launched by either worker.
- Clicking a current Output or Texture pixel fills the history coordinates and
  resource/subresource. This does not automatically run history or switch tabs.
  A resolved MSAA display seeds sample zero; the history selector always refers
  to an individual sample.
- The table shows original GPA IDs, native replay IDs, fragment indices,
  operation/test outcomes and before/after colors. CPU snapshots have no
  fragment index; integer colors retain their exact decimal integer values.
- Double-click a mapped row or use **Locate API**. Hidden API filters are cleared
  as needed. Unmapped rows cannot navigate to an invented command. The snapshot
  remains visible while navigating within the same frame and experiment.
- **Details** exposes the selected full record, or scope/gaps before selection.
  **Export…** atomically saves complete JSON including records, mapping/scope,
  frame hash, experiment/backend identity and the original query.

## Task and cache ownership

Both recapture and analysis use the main window's existing isolated worker,
Windows job object, timeout, cancellation and revision guards. Recapture finishes
before the analysis process starts. A failed/cancelled recapture never enters
the cache. A changed selection cannot publish an older in-flight result.

The cache retains up to four successful captures. Its identity includes the
frame hash, active experiment operations, replay driver, canonical RenderDoc
path and DLL content hash. Coordinate changes reuse the capture; experiment
edits use their own capture; undo can reuse a retained original. Cached files
belong to temporary directories owned by the window. Eviction/window destruction
releases those directories. Missing cached files trigger a fresh recapture.

Changing frame, effective experiment, driver, query fields or backend selection
invalidates displayed history as appropriate. Event navigation retains the
snapshot while updating the next query's API field. Missing backends fail with
a compact error and remain recoverable after selecting a valid library.

## Validation

The Qt harness loads all 22 independently validated backend history reports,
checks full row details and JSON exports, original command navigation, CPU
fragment absence, exact uint/sint formatting and stale-result rejection.

Actual main-window tests exercise Hardware and WARP recapture/analysis, known
MSAA uint values, coordinate-cache reuse, cancel/retry, disable/undo with cache
identity, API filter clearing, missing-backend recovery and selection changes
during analysis. A separate real CPU UpdateSubresource path exercises non-draw
query selection and navigation. Output pixel selection is also checked.

Release validation passed all **45 CTest suites**, including all **54 existing
Qt regression cases** without skips. After the final Details splitter/tooltip
adjustment, the complete Release build was repeated and the focused Qt harness
was run from `out/FloraGPA-pixel-history` with a Windows-only PATH: **6 passed,
0 failed, 0 skipped**. The harness used the packaged replay/history workers and
validated all 22 backend reports. Its temporary test executable and Qt Test DLL
were removed from the delivered package after completion.

The packaged CLI passed **4 golden-frame/negative controls** for GF2 and BF1.
An audit of **10 runtime reports** (three history jobs, three recaptures and four
golden runs) found no Python/Tk/GPA modules; Qt came from the package, and only
the explicit history paths loaded the selected installed RenderDoc DLL. All
four product executable hashes match the final Release build. The package does
not distribute RenderDoc or test binaries. Real CPU-history and MSAA screenshots
were visually inspected, including the expanded Details pane and exact uint
value 16777219.

Local evidence: `artifacts/ctest-history-ui-release.log`,
`build/vs2022/ui-Release.txt`, `artifacts/build-pixel-history-shipping.log`,
`artifacts/pixel-history-shipping.txt`, `artifacts/pixel-history-shipping/`,
`artifacts/pixel-history-golden/validation.json` and
`artifacts/pixel-history-runtime-audit.json`. Full CTest predates the final
splitter/tooltip-only adjustment; the packaged focused rerun includes it.

```powershell
# Run from the repository root; set external paths for your environment.
$ProjectRoot = (Get-Location).Path
$ResultsRoot = Join-Path (Get-Location) 'artifacts/results'
$env:FLORA_HISTORY_REPORTS = "$ProjectRoot/artifacts/rdc-history-package-final"
$env:FLORA_HISTORY_COMMANDS = "$ProjectRoot/artifacts/rdc-capture-package/commands.gpa_frame"
$env:FLORA_UI_ARTIFACT_DIR = "$ResultsRoot/history-ui"
ctest --preset release -R '^history_ui$'
```

The optional fixture corpus and installed RenderDoc are required for these GPU
cases; the harness reports skips when absent. Run GPU tests serially. The 1.45
ABI limitation and backend validation limits remain as documented in
[RDC_HISTORY_MIGRATION.md](RDC_HISTORY_MIGRATION.md).

## Remaining migration

This completes the Pixel History UI path covered above, not the shared advanced
analysis panel or the full Python application. RenderDoc-backed pixel/vertex/
compute debugging, counters and other shared worker consumers still require
migration. The malformed structured-chunk/failed-PickPixel injection tests and
separate clean-machine deployment verification also remain outstanding. Shared
module entries remain `partial`; no disabled debugger stage is claimed complete.
