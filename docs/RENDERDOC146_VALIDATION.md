# RenderDoc 1.46 compatibility test — 2026-09-23

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

This is an isolated test, not a change to the shipped 1.45 backend or its version
allowlist. All experimental source copies, headers, binaries and captures are
under ignored `build/renderdoc146/`. Experimental sources and binaries are not
included in the repository; this document records the validation results.

## Runtime and build

- DLL: `C:/Program Files/RenderDoc/renderdoc.dll`, file version `1.46.0.0`.
- Runtime reports release version `1.46`, commit
  `e4bd23b671d3d5a747ff5221dbe08a63eb6ca200`.
- DLL SHA256: `809da38e3867d9fd09cc5c30dd5310500dee75e999166d7a14ad1ef6e9eca65d`.
- Compiled the existing native analysis sources against the official
  [v1.46 replay headers](https://github.com/baldurk/renderdoc/tree/v1.46/renderdoc/api/replay).
  Kept the existing C++20 PointerVal initialization and dynamic allocator bridges.
  The test backend requires exactly the 1.46 release; the isolated Qt test copy
  changes its file-version guard to 1.46. No version guard was removed.
- Qt tests link the current Release libraries and the isolated PixelHistoryView
  copy; the existing native capture worker is used unchanged. Python was used
  only to prepare development files, never as an application backend.

## Passed checks

1. Independently recaptured BF1 with RenderDoc 1.46. Replay pixels retain golden
   SHA256 `1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6`.
2. Native Pixel History: resource 239, through GPA event 30072, pixel (1704, 955),
   mip/layer/sample 0. Seven records map to GPA events 29062, 29113, 29135, 29151,
   30033, 30053 and 30072; zero unmapped records and no CPU-write gaps.
3. Existing `HistoryUiTests::mainWindow(hardware)` and `(warp)` pass, including
   exact MSAA integer values, recapture reuse, cancellation/retry, experiment
   invalidation/undo, filtered API navigation, missing-backend retry and rejecting
   a result after changing API selection.
4. Real BF1 Qt mouse picking automatically queries Pixel History and returns the
   same seven records. Rapid consecutive clicks publish the last selected pixel
   and reuse the same recapture. This test passes with no skips.
5. Backend module inventory contains the selected RenderDoc DLL and native
   dependencies; no Python/Tk/GPA runtime is loaded by the analysis backend.

The first BF1 mouse test assumed an exact source pixel could be selected while
showing the 1920px image at less than 1:1 scale. Integer viewport rounding selected
(1703, 956), so that assertion failed. The corrected exact-pixel test uses 100%
zoom and pans to the target; it passes. This was a test-coordinate assumption,
not evidence of a replay ABI failure. The initial test harness also had a typo
in its temporary version-guard replacement; it was corrected before these runs.

## Evidence and limits

- `build/renderdoc146/validation.json`: DLL identity, native results and golden hash.
- `build/renderdoc146/ui-tests2.txt`: hardware/WARP passes and initial BF1 coordinate failure.
- `build/renderdoc146/bf1-ui-tests.txt`: corrected BF1 mouse/cancellation/cache test passes.
- `build/renderdoc146/ui-evidence/bf1-pick.json`, `bf1-pick-latest.json`, `bf1-pick.png`.
- `build/renderdoc146/bf1-recapture/independent_capture.rdc`: independent BF1 recapture.

This establishes a tested 1.46 path for these Pixel History scenarios. It does
not establish compatibility of the existing 1.45 binary with 1.46, nor parity
for every replay consumer (debugger, counters, mesh, asset export, other APIs or
all captures). The previous portable review package still checks for 1.45;
formal 1.46 integration and repackaging are separate from this test.
