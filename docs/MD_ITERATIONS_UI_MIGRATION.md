# Scheduled Intel metrics in Qt

The right-side **Inspector > Intel Metrics** panel migrates
`md_iterations_ui.py` into Qt Widgets. It uses the existing native scheduled
collector and offline result verifier. No Python, Tk or GPA runtime is loaded.
The layout keeps the API list on the left, frame inspection in the center and
metrics on the right. Explanations and complete records live in tooltips and a
collapsed Details view.

## Workflow and parity

- **Catalog** reads the installed Intel Metrics Discovery catalog in an isolated
  native worker. Available lists unique symbols and units; multiple selections
  can populate the requested symbols. Catalog identity follows the bridge path.
- **Setup** preserves symbols, all/selected FrameFile ranges, sample and warmup
  counts, requested pass, pass mapping and optional cached weight JSON.
- **More (...) > Preview plan** shows compatible sets and actual runs per pass.
  Only mapped pass 0 repeats. All passes and nonzero passes execute one outer
  iteration. Automatic weights require a separate replay; cached weights do not.
- **Measure** freezes the request, capture hash and experiment before starting
  the worker. A ready-file handshake prevents acquisition before assignment to
  a Windows Job with kill-on-close. Cancel terminates the worker and allows retry.
- **Values** groups the original matrix by range and shows medians and units.
  Unmeasured cells remain blank (`—`); unavailable measurements remain `NA`.
  Tooltips retain full numeric precision and valid/total sample counts. Details
  retains the original cell, request identity and raw report names. Display
  rounding does not modify exported values.
- Start/end API navigation uses the selected cell's recorded boundaries.
  Double-click locates its start. Experiment changes retain the matrix with a
  **Previous experiment** label. New captures clear the matrix; configuration
  edits preserve its original request identity. Failed/cancelled acquisition
  and failed catalog refresh preserve the previous accepted result.
- **Export ZIP** preserves raw reports, catalog, frozen experiment, publisher
  JSON/CSV and scheduler audits, plus process-tree, worker log and cached weights.
  Executable/DLL files and paths outside the result directory are excluded.

`MdIterationSession` validates requests and accepts results only after comparing
the frozen selection, metrics, repetitions, mapped pass, cached weights, frame
hash and experiment bytes. It invokes the full native offline verifier before
publishing the matrix. Experiment projects save the native settings and the old
Python `hardware_metrics.metric_iterations` fields. Legacy Chinese scope/pass
values are recognized; unsupported scopes require an explicit range choice.

The `metric-catalog --out <new-directory>` CLI/worker command also works without
a capture. That batch migrated the list-sets branch of `md_profile.py`; its
uniform collector is covered by the subsequent
[MD_PROFILE_MIGRATION.md](MD_PROFILE_MIGRATION.md). The shared `hardware_metrics_ui.py` and
`session.py` remain partial: other profiling owners and consumers are not
represented as finished. Settings are saved with experiment projects; this
does not claim that the entire Python application/session persistence is ported.

## Verification

`FloraScheduledMetricsUiTests` covers settings round trips, legacy settings and
out-of-domain counts, frozen request rejection, saved result acceptance and
identity mismatch, then exercises the real MainWindow and Intel worker.
The UI scenario reads a catalog, selects metrics, previews two passes, collects
all passes, navigates an API, exports, changes mapping, rejects a bad bridge,
cancels, retries with cached weights and saves/reopens experiment settings.
It verifies that failures and experiment changes preserve earlier results.

The packaged suite runs with only Windows system directories on PATH, with
seven cases passing and no skips. The exported ZIP is CRC-checked and loaded
by the unchanged Python `load_scheduled_result` reader. Publisher data agrees
exactly with the native result, including unmeasured cells, and the exported
profile agrees with the visible matrix. The final cached collection has three
raw reports and twelve cells; its process audit confirms Job assignment.
Worker module reports contain no Python/Tk/GPA modules and load Qt from the
package directory. Values, Setup and Plan screenshots are inspected at 1500x950.

The complete Release build and fourteen related CTest suites pass. The original
MainWindow suite passes 54 cases without skips. The first counter UI run skipped
three external-fixture cases; after configuring the recorded corpus and source
capture, all six counter UI cases pass without skips. GF2/BF1 golden frames and
both disabled-draw negative controls pass in the final package. All four shipped
executables and the metrics bridge match the Release build hashes.

Local evidence (ignored by Git):

- `artifacts/build-scheduled-ui-delivery.log`
- `artifacts/ctest-scheduled-ui-final.log`
- `artifacts/ctest-scheduled-ui-counter-regression.log`
- `artifacts/scheduled-ui-delivery-tests.txt`
- `artifacts/scheduled-ui-delivery/` (screenshots, matrix and ZIP)
- `artifacts/scheduled-ui-delivery-export/validation.json`
- `artifacts/scheduled-ui-delivery-golden/validation.json`
- `artifacts/scheduled-ui-delivery-audit.json`

To reproduce the package test, copy the test executable and Qt6Test.dll beside
the packaged application, then set these development-only fixture variables:

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
$env:FLORA_TEST_INTEL_METRICS = '1'
$env:FLORA_TEST_REFERENCE_ROOT = 'D:/captures-and-python-reference'
$env:FLORA_TEST_SCHEDULED_RESULTS = 'D:/results/md-iterations-delivery-parity'
$env:FLORA_UI_ARTIFACT_DIR = 'D:/results/scheduled-ui'
./FloraScheduledMetricsUiTests.exe
```

`tools/validate_scheduled_ui_export.py` checks the resulting ZIP against the
Python reference. These development tools are not application dependencies.

## Limits

This is a migration of the recovered scheduled workflow. It retains
`complete_original_scheduling: false`; it does not claim full original GPA GPU
scheduling or general adapter/driver compatibility. Intel metrics are collected
on the Intel adapter, independently of the main replay's hardware/WARP choice.
The earlier intermittent BF1 single-pixel mismatch remains unresolved and the
exact replay-image guard remains enforced. See
[scheduled collection](MD_ITERATIONS_MIGRATION.md) and
[offline validation](MD_ITERATION_RESULTS_MIGRATION.md).

Delivery: `out/FloraGPA-scheduled-metrics-final/FloraGPA.exe`; distribute the whole
directory. Test helpers are removed from the delivered directory after checks.
