# Qt coverage inspection

The central **Coverage** tab connects the existing native coverage executor to
the Qt analyzer. Its placement retains the GPA-style API log on the left,
image inspection in the center and resource properties on the right. Controls
occupy two compact toolbars; limitations remain in tooltips and the exported
report. No timing values or unsupported analysis features are fabricated.

## Original behavior and native implementation

| Original Python responsibility | Native implementation |
| --- | --- |
| `app.coverage`: draw, fragment/geometry, effective target, layer and depth selection | `CoverageView` and `MainWindowCoverage`; the isolated worker receives the selected event, current experiment and hardware/WARP setting |
| `coverage.export_coverage`, `_coverage_bound`, `_write_coverage` | `captureCoverage` owns the replay boundary; `exportCoverage` preserves the full report, mask, after-draw image and overlay |
| `coverage_fragment`: blend handling, target/view bounds, dimensional marker allocation, selected storage and original/isolated submission | `CoverageCapture`, `outputSubresource`, native readback and private output bindings; no Python execution |
| `coverage_geometry`: replacement shader and read-only depth/stencil | `compileMarker` and `CoverageCapture::depthState` |
| `coverage_viewport`: extent, original array routing, UAV relocation and neutral background | `viewportTarget`, `relocate`, marker binding and viewport report construction |
| `widgets.ImageView`: fit, actual size, dimensions, pan/zoom and pixel selection | Shared Qt `ImageView`, Fit / 1:1 actions and dimensions/zoom label |
| `app.link_pixels`: resource/event/mip/layer/sample selection | Native Pixel History and pixel debugger selection, with the exact resource and subresource shown in the inspector |
| `app.link_buffer_pixels`: view-relative element to byte range | Buffer inspector selects `(first_element + x) * element_size`, one element, after the selected event |
| Viewport coordinates have no resource target | Pixel navigation is disabled for viewport results |

Target choices include only effective bound RTV slots and DSV, excluding UAV
slots. Empty layer means the original default texture layer or all viewport
indices. Explicit values are unsigned 32-bit indices; view bounds remain a
backend validation. Failed requests clear stale output and allow retry.

Changing the frame, API event, experiment, device or coverage controls invalidates
the old image and navigation. Request revisions reject stale results. Cancel uses
the existing native worker process/job cancellation. Export keeps its own copies
of the four artifacts, so subsequent worker-directory cleanup cannot invalidate
an accepted result. ZIP is only a container for those existing artifacts.

The Python `save_ui` document does **not** persist the coverage mode, target,
layer or depth checkbox. This migration preserves that behavior. Existing
experiment edits, undo/redo and device settings continue through the shared
application session rather than a separate coverage experiment model.

## Verification

The backend evidence remains in [COVERAGE_EXECUTION_MIGRATION.md](COVERAGE_EXECUTION_MIGRATION.md):
752 independent Python/native GPU cases and 6,704 checks, including real GF2/BF1
draws, complete reports, exact image bytes, counters, SO history and MSAA storage.
No backend implementation was changed for the Qt integration.

`tests/CoverageUiTests.cpp` exercises controls, invalid indices, stale requests,
owned export data, native worker failures and retry, cancel and retry, experiment
disable/undo, geometry without depth tests, WARP, texture pixel navigation,
buffer byte-range navigation and targetless viewport behavior. Its real GF2
case captures the final draw, exports all artifacts and renders the full window.
`tools/validate_coverage_ui_export.py` compares that ZIP with the independent
original Python result: complete JSON equality and exact decoded pixels for
all three PNGs.

Delivery results:

- Complete Release build passed (`artifacts/build-coverage-ui-final.log`).
- Seven relevant CTest suites passed: coverage UI, coverage execution, original
  UI, pixel-history UI, texture-edit replay, frame output and GPU profiling
  (`artifacts/ctest-coverage-ui-final.log`). The original UI suite has 54 passes,
  no failures or skips; Coverage has six passes including setup/cleanup and the
  real frame, no failures or skips.
- The deployed package passed the same six Coverage UI cases under a
  Windows-only PATH (`artifacts/coverage-ui-shipping.txt`). Its screenshot
  `artifacts/coverage-ui-shipping/coverage.png` was inspected at 1600 × 960.
  Qt's offscreen font-directory and size-hint warnings did not prevent readable
  rendering. Temporary Qt Test DLL and test executable were removed afterward.
- All four exported artifacts match the independent Python GF2 draw-1508
  result, for both build and deployed worker executions
  (`artifacts/coverage-ui-export-final/validation.json` and
  `artifacts/coverage-ui-export-shipping/validation.json`).
- The deployed CLI/worker passed 16 success/rejection cases
  (`artifacts/coverage-ui-cli-final/validation.json`).
- Four golden-frame/negative controls passed, preserving the known GF2/BF1
  image hashes (`artifacts/coverage-ui-golden-final/validation.json`).
- Ten runtime reports contain no Python/Tk/GPA/RenderDoc runtime modules and
  load Qt from the package. All four product executables match Release hashes
  (`artifacts/coverage-ui-runtime-audit.json`).

Delivered application: `out/FloraGPA-coverage-ui/FloraGPA.exe`. Distribute the
whole directory, including its native worker, Qt plugins and VC runtime DLLs.

## Scope and limits

The four standalone coverage modules are audited independently of the shared
`app.py`, `widgets.py`, replay and private-output modules. Shared module status
does not become complete merely because this consumer works. In particular,
Quad execution and its UI still require migration.
The four audited standalone coverage entries now have status `ported`;
`coverage_isolated.py` remains `partial` for its remaining consumers. Overall
module counts are 20 ported, 116 partial and 68 pending; these are not a
work-weighted completion percentage.

The backend's original limitations remain visible in reports/tooltips: sample
union rather than overdraw counts, unreconstructed pre-capture MSAA samples,
separate-draw ordering limits and buffer coordinates that are not observed
write addresses. Validation does not establish arbitrary-capture support or
deployment on a separate clean Windows installation.
