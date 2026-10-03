# Draw resources workspace

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

Implemented with C++20 / Qt Widgets and the native isolated worker. No Python
runtime, GPA injection or changes to the reference workspace are involved.

## Interaction

`Resources` replaces the Output, Texture and Coverage tabs. The resource rail
shows Outputs first so all bound RTs remain easy to find, followed by Inputs;
non-texture input buffers are collapsed. Rows keep stage/slot, resource ID,
view format, dimensions and independent bindings. Counts distinguish unique
input textures, SRV bindings, RTVs, DSVs and UAVs. These are effective bindings,
not evidence of actual shader sampling or writes. Unresolved rows keep an error.

The lowest valid RTV is selected by default, then DSV, then a previewable input.
`Final Frame` is a separate action and is excluded from the draw's RT count.
Global texture selection enters this same workspace. The existing output and
texture widgets share one visible viewer area, retaining their storage export,
channel/range, format, mip, layer/slice and sample controls.

`Coverage` defaults off. When enabled it follows the selected draw and output,
uses fragment coverage with depth/stencil testing, and adds a separate magenta
mask at fixed 100% opacity; no opacity control is shown. Reading a pixel still reads the base image. Inputs, Before
and Final views pause the overlay. Resource, view and subresource must match;
changing selection clears the previous mask immediately. MSAA is labelled as
the union of covered samples. Advanced settings retain geometry, ignore-depth,
viewport diagnostics and the existing exports.

Enable `Pixel History`, then click an RT pixel. A marker appears and the history
panel beside API Log opens. Dragging pans without querying. Queries use the
resource/subresource and execution boundary of the displayed report. Resolved
MSAA displays query an individual sample (default 0). `Coordinates` contains the
manual controls; `Details` exposes the selected record including before/after
values. Double-click navigates to the API while preserving the result snapshot.
The panel reports the queried pixel, resource, sample and API upper bound.

RenderDoc must pass the existing 1.45 release backend requirement. The UI checks
the file version before recapture; the native backend still performs its own
compatibility checks. Use `RenderDoc…` to select a library and click again.
The installed 1.43 DLL is not silently loaded for history.

## Worker and cache

`src/application/DrawResources.*` defines bindings, image selections and request
contexts. It reuses effective bindings and edited view descriptions.
`src/app/ResourceBrowser.*` owns the inventory and thumbnail cache;
`MainWindowResources.cpp` coordinates the existing serialized worker channel.
Selection waits 200 ms. Main previews and queued pixel queries take precedence
over background thumbnails. Cancellation and context checks discard stale work.

Example:

```powershell
FloraGPA.Cli.exe draw-resources capture.gpa_frame --id 1000 --out previews --preview-request request.json
```

```json
{"previews":["in/PS/SRV/0","out/OM/RTV/0"]}
```

Without a preview request the command returns inventory only. `--experiment`,
`--warp` and `--debug-device` retain their existing meanings. A request contains
at most 16 known binding keys. One prefix replay observes the prepared input
boundary before executing the draw and the output boundary after executing it.
Preview errors are attached to individual rows. Shared previews are deduplicated;
images retain aspect ratio with a maximum side of 96 pixels. Only selected and
visible rows are requested. The decoded thumbnail cache is capped at 128 MiB;
worker files are removed after loading, rather than kept alongside cached images.
Keys include capture identity, effective experiment, device, event, binding/view
and displayed subresource. Existing CLI commands and export formats are retained.

Optional experiment UI field `flora_resources` stores binding and the Coverage toggle.
Legacy opacity fields are ignored. Old projects still load; the initial workspace falls back to
Resources. Saved dock structure and unrelated analysis pages are retained.

## Validation on 2026-09-23

Release built with VS2022 x64 and Qt 6.11.2. GPU work ran serially.

- Eight relevant CTest suites passed: draw_resources, history_ui, coverage_ui,
  coverage, texture_inspector, ui, frame_output and replay_pipeline.
- New synthetic tests cover duplicate/stage bindings, sparse/no RTV, depth/UAV,
  buffer views, view bounds and edited SRVs, uint64 identifiers, unknown requests,
  Before/After readback, base-image preservation, scaled pixel mapping, drag
  suppression, automatic history requests, stale history/thumbnail rejection,
  missing backend, Coverage selection changes and rapid RT switching.
- General UI suite: 45 passed, 9 skipped for missing external fixtures. Coverage
  UI: 5 passed, 1 skipped for external parity evidence. History UI has only its
  setup/cleanup pass; all four backend-dependent cases skip. New request/picking
  tests run in draw_resources. These results are not full parity.
- BF1 capture: `E:/captures/BF1_GPA_Captures/bf1_2026_01_21__16_53_05.gpa_frame`.
  Event 876 exercises four RTVs, a DSV, real thumbnail loading and input/RT
  switching. Event 30072 produces 996 depth-tested covered pixels on resource
  239. Screenshots exercise 100%, 150% and 200% Qt scale factors (offscreen
  rendering with Windows fonts); physical multi-monitor DPI transitions were
  not tested. The same BF1 tests also passed inside the portable directory with
  system-only PATH at all three scales; the temporary test executable/DLL were
  removed afterwards. All four executables and the metrics DLL match Release
  hashes. Evidence is under ignored `build/resources-review/`.
- BF1 full replay matches SHA256
  `1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6`.
  Suppressed-draw negative control differs, with 0 draws / 73 dispatches. Native
  module audit reports no Python, GPA, Tk or RenderDoc runtime for these replays.
- RenderDoc 1.45 and the GF2 golden capture are unavailable on this machine.
  Real Pixel History, RenderDoc recapture/cache retry end-to-end and GF2 golden
  parity are **not verified**. Version rejection, request routing and stale-result
  handling are tested separately; this does not substitute for a real history run.

Latest portable review output: `out/FloraGPA-Resources-Review-FixedCoverage/FloraGPA.exe`
and `out/FloraGPA-Resources-Review-FixedCoverage-Windows-x64.zip`. The fixed-opacity
follow-up passed the draw_resources and coverage_ui suites and the real BF1
screenshot test. Generated packages and validation artifacts remain outside Git.

Follow-up: [RenderDoc 1.46 isolated validation](RENDERDOC146_VALIDATION.md)
now verifies real BF1 mouse-driven Pixel History plus hardware/WARP history
regressions using a separately compiled matching backend. The original package
and its 1.45 allowlist remain unchanged.

## Local import verification — 2026-10-02

The takeaway archive at commit `f1c3e82` was verified as a direct descendant of
the local `a746005` baseline. ZIP CRC/path checks, Git object integrity and the
clean archived worktree were checked before integration. No incoming Git config,
hooks or generated build directories were copied into the main checkout.

A fresh VS2022 / Qt 6.11.2 Release build produced the application, native workers,
metrics bridge and nine relevant test targets. Eight CTest suites passed on the
first run; `history_ui` exposed an outdated test selecting the central tab even
though Pixel History now belongs to the left tab group. The test now locates the
owning tab group and verifies visibility. Its corrected rerun passed all six
cases without skips, including CPU writes and real RenderDoc 1.45 hardware/WARP
history, cancellation/retry and cache behavior. CTest now retains a dedicated
history text log. No production behavior was changed by this test correction.

The main UI passed 54 cases, draw resources 10 (including BF1 screenshots), and
Coverage UI six, all without skips. Coverage, texture inspection, frame output,
replay pipeline and core suites also passed. GF2/BF1 golden replays and both
suppressed-draw negative controls passed from the portable package with an
isolated child PATH. All five production binary hashes match the reviewed build.
These are focused regression results, not a rerun of all 88 registered suites.

Evidence, including the initial failure and corrected history run, is retained
under `artifacts/takeaway-review-20261002/`. The resulting package is
`out/FloraGPA-resources-20261002/`. RenderDoc 1.46 production support remains
outside this import; the separate clean-machine validation limit also remains.
