# Native Qt Quad view

`QuadView` and `MainWindowQuad` migrate all responsibilities of Python
`quad_ui.py`. The Quad tab sits beside Output and Coverage in the central
inspection area, retaining the API log on the left and properties on the right.
Two compact toolbars provide capture/cancel/export, depth mode, coordinate
target, layer/index, summary/accounting status and Fit / 1:1. The nine original
color bands sit above the heatmap; a footer displays the selected cell.
The full JSON report has its own tab. Depth explanations, measurement limits
and detailed provenance remain in tooltips and exported data.

## Original behavior retained

| Python behavior | Native implementation |
| --- | --- |
| Prepared / pre-draw / disabled diagnostic depth | `quadDepth` passes the original `prepared`, `before`, `none` modes to the native worker |
| Auto, RT0–RT7, depth/stencil, optional layer/slice/index | Original target choices and uint32 input validation; native backend validates the selected view |
| Original experiment and selected event | Shared MainWindow worker supplies current experiment edits and hardware/WARP setting |
| Raw uint32 values on click | Little-endian counts remain independent of the preview color band; values through UINT32_MAX display exactly |
| Source coordinate cells | View-bounded 2×2 source range, clamped at odd extents |
| Buffer cell coordinates | Two view elements per cell, with original first-element offset and exact byte range; tooltip preserves the distinction from observed write addresses |
| Scope and accounting | Actual group/reference counts; explicit accounting mismatch; original limitations and target/sample/driver/submission provenance in Report/tooltips |
| Raw report and experiment key | Complete native/Python-equivalent report plus the captured experiment/device key |
| Settings | Original flat `ui.quad_depth`, `ui.quad_target`, `ui.quad_layer` fields round-trip through experiment save/load; a new capture restores defaults |
| Diagnostic ZIP | `result.json`, five `data/*.u32le` arrays and `data/quad_counts.png`, matching the original seven-member layout |

Changing the capture, selected event, experiment, adapter or Quad controls
invalidates the displayed result. Request revisions reject stale completions.
Cancel uses the native worker process/job cancellation and permits retry.
Accepted output owns the report and all exported bytes, so later temporary
directory cleanup cannot invalidate export. No Python runtime participates in
the view, worker or export.

The shared native ZIP writer previously accepted flat filenames only. It now
accepts relative forward-slash member paths, preserving the original Quad
`data/` layout. Empty/dot/parent segments, absolute/drive-qualified/backslash
paths, NULs and duplicate members are rejected; QSaveFile preserves an existing
archive when a write fails. Existing flat archive consumers retain their layout.

## Verification

Backend evidence remains in [QUAD_EXECUTION_MIGRATION.md](QUAD_EXECUTION_MIGRATION.md):
582 synthetic hardware/WARP cases including 14 original refusals, 12 real
GF2/BF1 cases and 24 CLI/worker integration checks. The diagnostic algorithm and
replay implementation did not change for this UI migration.

`tests/QuadUiTests.cpp` covers actual native capture and worker execution, full
unsigned cell values, odd texture/buffer/viewport coordinates, invalid indices,
truncated output refusal, accounting mismatch presentation, stale results,
owned ZIP bytes, archive path/transaction checks, disable/undo, failure/retry,
cancel/retry, original experiment setting round-trip, WARP and event changes.
Its real GF2 test captures DrawIndexed 113 and renders the complete window.

The initial run exposed the flat-only ZIP restriction; it did not reveal a
counter or image-loading mismatch. After relative-path support, all seven Quad
UI cases pass without failures or skips, including setup/cleanup and the real
capture (`artifacts/ctest-quad-ui-v2.log`, `build/vs2022/quad-ui-Release.txt`).
The Windows-platform screenshot `artifacts/quad-ui-v2/quad.png` was visually
inspected. Native fonts render correctly and the view contains no explanatory
text blocks.

`tools/validate_quad_ui_export.py` verifies all seven members, CRCs, complete
report equality (apart from the explicit UI experiment key) and exact bytes for
all five arrays and the PNG. Both synthetic and real GF2 exports match the
previously verified backend outputs (`artifacts/quad-ui-v2/zip-validation.json`).

Delivery checks:

- Complete Release build passes (`artifacts/build-quad-ui-final.log`).
- Four additional CTest suites pass: checkpoint UI/archive transactions,
  Coverage UI, GPU profiling and the original main-window suite
  (`artifacts/ctest-quad-ui-regression.log`). The original suite has 54 passes,
  no failures or skips.
- The deployed package passes all seven Quad UI cases without failures/skips
  under a Windows-only PATH, using its own worker and Qt runtime
  (`artifacts/quad-ui-shipping/tests.txt`). Its Windows-platform screenshot
  `artifacts/quad-ui-shipping/quad.png` was inspected. The temporary test
  executable and Qt Test DLL were removed from the delivered directory.
- Both deployed ZIP exports pass exact seven-member comparisons
  (`artifacts/quad-ui-export-shipping/validation.json`).
- The deployed CLI and Worker pass all 24 success/error/experiment checks
  (`artifacts/quad-ui-cli-shipping/validation.json`).
- Four deployed GF2/BF1 golden/negative controls retain original image hashes
  and command counts (`artifacts/quad-ui-golden-shipping/validation.json`).
- Fourteen runtime reports load Qt from the package and no Python/Tk/GPA/
  RenderDoc modules. All four product executable hashes match Release
  (`artifacts/quad-ui-runtime-audit.json`).

Delivered application: `out/FloraGPA-quad-ui/FloraGPA.exe`. Keep its entire
directory together, including the native worker, plugins and runtime DLLs.
This validates an isolated environment on the current host, not another clean
Windows installation.

## Scope

This completes the original Quad panel, not the entire Python application.
Shared UI/session/image/private-output modules retain remaining consumers and
must be audited separately. The recovered bounded-lock counter and original
scheduling/MSAA/discard/depth/buffer-coordinate limitations remain unchanged;
the UI reports diagnostic groups, not physical quad invocations.

`quad_ui.py` is now marked ported. The module ledger is 29 ported, 115 partial
and 60 pending; these are module counts, not a workload completion percentage.
