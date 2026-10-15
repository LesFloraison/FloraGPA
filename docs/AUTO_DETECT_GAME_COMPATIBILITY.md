# Auto-Detect: POOLS and HELLDIVERS 2

This follow-up separates attachment, HUD rendering, frame publication and actual
GPU replay. It does not promote the earlier three-game acceptance matrix to full
parity, or replace the existing GF2 fidelity findings.

## Causes and changes

Both applications reached the native DX11 runtime through Auto-Detect. The visible
failures occurred after attachment:

* POOLS keeps TIMESTAMP_DISJOINT queries active across Presents. Timing queries
  now permit isolated HUD/readback work; occlusion, pipeline-statistics and
  predicate queries retain their guards. GenerateMips is recorded and replayed.
* HELLDIVERS 2 uses a two-buffer DISCARD swap chain. Only buffer zero is accessible
  for this presentation model; clean storage now uses one logical buffer, without
  assuming retained post-Present contents.
* HELLDIVERS 2 submits immediate-context calls from worker threads. Recording
  accepts these only while the generated proxy owns the same device gate. Raw
  foreign-thread observations, different contexts and unproven boundary mappings
  still fail explicitly.
* HELLDIVERS 2 requests DX11 feature level 12.1 and uses D24S8 MSAA initial depth.
  Replay preserves the captured feature-level requirement. D24S8 sample restoration
  reconstructs depth and stencil per sample; 2x/4x, two-array-layer fixtures compare
  against native output and the original Intel player.
* Ordinary EVENT/TIMESTAMP/TIMESTAMP_DISJOINT observations retain descriptor and
  End identity. A private EVENT at each recorded End and a real GetData wait at
  each matched successful observation restore the GPU completion dependency,
  including preceding uploads. Pre-frame work is covered by the recorder's own
  boundary completion. Legacy timing payloads retain only the first UINT; original
  GPU timing values are not reconstructed.
* Mapping checks now visit a weak index of mapped resources, and HUD capability
  checks read a count instead of constructing the full object-status JSON.
* Qt polling pauses while a capture selector is open and preserves unchanged
  swap-chain entries, so background status updates do not dismiss the popup.

No game-name allowlist, launcher patch or original Intel runtime dependency is
introduced. Steam remains running before Auto-Detect is enabled.

## Capture memory budget

POOLS' first chapter produced a 1,880,524,371-byte frame, exceeding the default
1 GiB resource budget. Select **Capture limit: 4 GiB**, then **Capture Next Frame**
in DX11 Capture. The selector applies to button requests; HUD hotkeys retain the
default 1 GiB limit. The equivalent CLI option is `--resource-limit-mib 4096`.

## Validation records

Generated captures, screenshots, logs and comparisons remain in ignored
`artifacts/auto-detect-pools-hd2-*` directories. The tracked machine-readable
baseline records final results and limitations. Interim failures are retained.

The final portable GUI (`artifacts/FloraGPA-AutoDetect-Compatibility-20261016-r2`)
was started with only Windows directories on PATH. With Application empty,
Auto-Detect attached POOLS launched from the existing Steam library, displayed
the HUD, published a 4 GiB-budget capture, and opened and executed it in the GUI.
The packaged CLI independently replayed the same frame: 115 draws, 163 dispatches,
two GenerateMips calls and 2560 × 1440 output; reference pixels were not used.
Fifteen pixels differ from captured output by at most one 8-bit level.
Auto-Detect was then stopped; both AppInit registry views match their saved values.

HELLDIVERS 2 intro replay is byte-exact. The ship replay executes 1,128 draws and
94 dispatches, with 57,552 differing pixels, maximum channel error 9 and mean
absolute RGBA error 0.00605048 on a 0–255 scale. The original Intel player also
differs from the captured ship output; the cause has not been established.
Draw suppression produces a different image, confirming executed rendering.
These are scene-specific results, not full-game fidelity claims.

Validation includes a full VS2022 x64 Release build, five query/D24 CTests,
18 rendered control cases, GF2/BF1 golden frames and their negative controls,
and 15 passing Qt cases. Of 42 selected regressions, two initially timed out
while the full build was also active; both passed unchanged in a later serial
rerun. Initial logs remain available. See
[`auto-detect-game-compatibility.json`](auto-detect-game-compatibility.json)
for hashes, paths, per-scene results and unresolved scope.

Development-only reproducible checks:

```powershell
python tools/validate_capture_timing.py --bin build/vs2022/Release `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/new-query-validation `
  --original D:/CDXrepo/FloraGPA/output/query-capture-v1/queries.gpa_frame
```

The original query capture SHA-256 is
`241e26d3ed1774d5ec10c6ee2599bf539b3c12e9f2bc4bd98d4b68eca6eb4d99`.
Its 41-byte GetData records with DataSize 8 and 16 establish the legacy first-word
layout. The new producer does not load that capture or any Intel component.

Relevant platform semantics: [DXGI swap effects](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_effect),
[DX11 feature levels](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-devices-downlevel-intro),
and [D3D11 queries](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query).
