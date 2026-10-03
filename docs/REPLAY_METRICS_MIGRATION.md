# Native replay metrics

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

The right-side Inspector now includes **Replay Metrics**, following the frame
analyzer's API-log / output / metrics layout. This migrates the original
`rdc_worker.py` counters action and its `advanced_ui.py` consumer.

## Behavior

- **Measure** independently recaptures the current frame and experiment, then
  queries the explicitly selected RenderDoc 1.45 native replay controller.
- All available counter descriptions, categories, UUIDs, types, widths and units
  are preserved. As in Python, only generic counters below `FirstAMD` are fetched.
- **Selection** shows the selected GPA event; **Frame** groups the complete
  results by replay event. Filtering and **Locate** / double-click navigate back
  to API Log. Unmapped replay events remain identified by their RDC event IDs.
- The **Available** tab shows the counter catalog. **More** contains the backend
  selector and collapsed JSON details. **Export** saves the entire result,
  regardless of the current filter or selection.
- Integer values remain exact through native decoding, display and export,
  including values above the precision of a double. Float widths, nonfinite
  string representations and negative zero follow the Python serializer.

The worker and recapture cache are shared with Pixel History and VS/PS/CS
debugging. Frame, experiment, driver or backend changes invalidate measurement
results; changing the selected event retains the full-frame result. Requests
support cancellation and retry. Switching event while a worker runs cancels the
shared analysis job. Failed or stale jobs cannot restore an earlier result.

Code: `src/rdc/Counters.cpp`, the counters branch in `src/rdc/Backend.cpp`,
`src/app/RdcCountersView.cpp` and the shared `MainWindow` analysis pipeline.
Neither the application nor its workers execute Python, Tk or Intel GPA.

## Verification and measurement limits

`tools/validate_rdc_counters.py` runs the original Python worker only as a
development oracle. It compares counter catalogs, maps, selection, complete
row provenance and non-time values on commands, Hardware/WARP MSAA, disabled
events, before-event capture, GF2, BF1 and an explicitly selected draw.
GPU durations are separate measurements and are checked for finite,
non-negative values rather than equality.

Strict BF1 comparisons exposed PS invocation variability. Repeating the same
capture with the same implementation changed 28 C++ rows and 25 Python rows
between the initial two runs; other non-time counters stayed identical.
The strict failed reports are retained in `artifacts/rdc-counters-parity` and
`artifacts/rdc-counters-repeat-bf1`. This is observed repeat variability; its
driver-level cause has not been established.

The optional `--bf1-repeat-evidence` audit requires prior native and Python
results for the same capture, checks all stable fields exactly across four
reports, requires independently observed PS variability in both implementations,
and records every varying PS row separately. BF1 PS counts must remain valid
uint64 measurements; they are **not counted as exact numerical parity**. This
does not change, average or normalize the values returned by the application.
Omit that option to retain the strict cross-run comparison.

`FloraRdcCounterTests` exercises actual counter union decoding, widths, units,
UUIDs, extreme integers, negative zero and nonfinite values. The Qt harness
checks complete recorded reports, exact displayed/exported values, filter and
scope behavior, stale results and failed replacement. Real Hardware/WARP cases
exercise measurement, API navigation, shared shader-debug capture, cancel/retry,
disable/undo, missing-backend recovery and selection changes during work.

```powershell
# Run from the repository root; set external paths for your environment.
$CaptureRoot = 'C:/captures'
$ResultsRoot = Join-Path (Get-Location) 'artifacts/results'
$env:FLORA_COUNTER_REPORTS = "$ResultsRoot/rdc-counters"
$env:FLORA_DEBUG_SOURCE_CAPTURE = "$CaptureRoot/shader_sources/source.gpa_frame"
$env:FLORA_UI_ARTIFACT_DIR = "$ResultsRoot/counter-ui"
ctest --preset release -R '^rdc_counter(s|_ui)$' --parallel 1
```

Fixture-dependent checks explicitly skip if their variables are absent. GPU
checks and original Python comparisons must run serially.

The final corpus audit covers **9 comparisons / 18,928 result rows**:
**16,157 exact values**, **1,456 independently measured durations**, and
**1,315 BF1 PS measurements audited separately**. All catalog and provenance
fields match. The final repeat audit observed 29 native and 19 Python PS rows
changing relative to the first run; its full differences remain in
`artifacts/rdc-counters-verified/bf1-ps-variation.json`.

The complete Release build passed. All **7 relevant CTest suites** passed:
counter decoding/UI, worker input rejection, debug decoding/UI, Pixel History
UI and the original main-window regression. The latter ran **54 cases without
failures or skips**. Counter UI also ran from the delivered package with a
Windows-only PATH: **6 passed, 0 failed, 0 skipped**, including all nine reports
and real Hardware/WARP jobs. The initially discovered invalidation crash was
fixed by blocking tree selection signals while discarding the old result;
both final runs include that fix and the compact toolbar/column layout.

The delivered CLI passed **4 GF2/BF1 golden-frame and suppressed-draw controls**.
Auditing **10 runtime reports** found no Python/Tk/GPA modules; Qt came from the
package and RenderDoc appeared only for explicit recapture/analysis jobs.
All four delivered executable hashes match Release. Actual packaged Qt
screenshots were visually checked. The temporary test executable and Qt Test
DLL were removed from the package after validation. This is same-host isolation
evidence, not a test on a separate clean Windows installation.

Package: `out/FloraGPA-replay-metrics`. Evidence: `artifacts/build-replay-metrics.log`,
`artifacts/rdc-counters-verified/validation.json`,
`artifacts/ctest-replay-metrics.log`, `artifacts/replay-metrics-shipping.txt`,
`artifacts/replay-metrics-shipping/rdc-counters/`,
`artifacts/replay-metrics-golden/validation.json` and
`artifacts/replay-metrics-runtime-audit.json`.

## Remaining scope

This is the existing generic replay-counter workflow, not the unfinished Intel
vendor metric-set scheduler or GTPin path. A catalog entry does not imply that
its vendor-specific value was measured. Measurements are taken on the replay
GPU, not recovered timing from the original capture. RenderDoc remains optional
and is required only for these analysis operations; ordinary independent replay
does not load it. Shared inventory, texture and postmesh RenderDoc actions and
other incomplete consumers remain tracked in `docs/migration.json`.
