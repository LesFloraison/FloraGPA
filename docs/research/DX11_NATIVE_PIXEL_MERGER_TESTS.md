# Independent native pixel merger tests

Implementation: `8c6c1a532af9530bd8d66bec7859768d5167984e`.
This P5/P6 continuation adds measured per-storage-sample tests to the existing
private shader/output-merger joins. It does not promote those joins to original
fragments or enable public pixel debugging before invocation selection is proven.

## Behavior

`NativePixelMergerState::measureTests` requests four queries per storage sample:
independent depth, independent stencil, independent alpha-to-coverage, and the
combined late-test path. Each query reads the same initial color/depth/stencil
storage. Color write masks, depth writes and stencil write masks are disabled
during diagnostics, including for stencil-fail and depth-fail operations. The
normal merging draw then restores the supplied state and produces the actual
private output values.

The independent shader retains the supplied depth and output alpha, but removes
discard and `SV_Coverage`. Depth and stencil probes disable the other tests;
the alpha probe disables both depth and stencil. The combined probe retains the
shader's activity, coverage output, original coverage/sample masks and all three
tests. Every query selects exactly one storage sample. Counts outside zero or
one are rejected, and the sum of combined sample counts must equal the normal
merging draw's occlusion count.

The independent alpha result is a counterfactual when the original shader
outputs `SV_Coverage`: D3D11 disables alpha-to-coverage for that combined shader.
The validator accounts for this distinction rather than ANDing every independent
result indiscriminately. Hardware alpha-to-coverage patterns are measured at the
same target dimensions and pixel location, not approximated from alpha.
See Microsoft's [blending contract](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-blend-state).

The optional `test_observations` object has schema 1 and scope
`private_late_tests_at_initial_storage`. It records enabled tests, lane activity,
discard, coverage masks, per-sample inputs and each query's count/pass result.
Standalone callers default to no diagnostics; draw-history shader/merger joins
request them. The existing result backend remains `native-pixel-merger/4`; this
is an additive optional field. Original eligibility remains null and early-depth
reconstruction remains false.

The tests can independently reject both depth and stencil. They do not identify
which test ran first in the original draw. A passing query also does not imply a
changed value: write masks, blend results and existing values remain separate
from execution evidence. The existing unchanged-write cases continue to pass.

The probes share the merger's private device on the verified adapter. They need
no new target clones or application UAV slots. The extra query count is bounded
by four times the validated sample count, at most 128. Existing cancellation and
bounded GPU waits remain active; enclosing history jobs retain their overall
deadline and storage limits.

## Validation

All GPU work ran serially on WARP, NVIDIA vendor 4318 and Intel vendor 32902 with
the D3D11 debug layer. No tests were skipped or assertions relaxed.

| Run | Qt results | Elapsed |
| --- | ---: | ---: |
| Complete merger suite | 1,353 passed | 354.714 s |
| Cancellation/retry boundary follow-up | 3 passed | 0.147 s |
| Geometry shader/history/merger integration selections | 89 passed | 56.565 s |
| UAV checkpoint shader-history selections | 17 passed | 10.942 s |

Qt totals include initialization and cleanup. The complete merger evidence
contains 1,338 merger cases and 12 binding-selection reports. The new validator
checks all 2,496 storage-sample observations in those merger cases and rejects
6,690 altered reports. Existing fixture checks still compare final storage with
independent captured GPU draws. Depth tests additionally check all eight
comparison functions, front/back stencil comparisons and read masks against
fixture arithmetic. A zero color write mask is explicitly required to preserve
storage while reporting a passing sample; discarded shaders must fail combined
tests while independent tests remain observable.

The cancellation follow-up interrupts after diagnostic work has started, then
retries and compares the result with diagnostics disabled. Original replay
storage is unchanged. This extra test was added after the complete suite;
production sources did not change between that suite and the follow-up. The
retained final executable includes the extra test. It is not presented as the
exact executable used for the earlier complete-suite run.

The 102 owned history cases and nine real-capture cases pass the existing join
validator, including 1,197 corruption controls. New observation validation covers
117 geometry joins, 45 UAV checkpoint joins and 36 real-capture joins, with
585, 225 and 180 additional corruption controls respectively. These overlap the
existing cases and are not additional GPU runs. All 36 real joins match their
private GPU checkpoints; final selected storage matches the separate ordinary
replay and original storage restoration checks pass.

| Capture/event | WARP ms | NVIDIA ms | Intel ms |
| --- | ---: | ---: | ---: |
| GF2 / 113 | 719 | 1,243 | 1,255 |
| BF1 / 2010 | 7,010 | 12,034 | 9,406 |
| Helldivers / 200 | 2,736 | 8,091 | 3,358 |

These are whole research-probe times, including replay/reference work. The prior
merger suite took 338.343 seconds for the same Qt count; the new run took 16.371
seconds more (about 4.8%). These runs were not a controlled performance benchmark.
No peak-memory, transient-module audit or standalone-release result is claimed.
GUI, CLI, Worker and the affected test/probe Release targets build successfully.

## Reproduction and evidence

```powershell
$env:FLORA_PIXEL_MERGER_EVIDENCE='<new-root>/merger'
build/vs2022/Release/FloraNativePixelMergerTests.exe -o <new-root>/merger.txt,txt
python tools/validate_native_merger_tests.py <new-root>/merger --out <new-root>/merger-validation.json
```

For history integration, run the geometry test slots `mergerHistory`,
`shaderHistory`, `shaderHistoryBoundaries`, `shaderHistoryRecovery`, and the
checkpoint test slot `shaderHistory`. Set their existing evidence environment
variables to separate new directories. Run `validate_native_pixel_history_merger.py`
with those directories and the nine real-probe prefix, then run
`validate_native_merger_tests.py --history` on each evidence directory.

Raw evidence and final binary snapshots are under ignored
`artifacts/dx11-native/merger-test*` paths. The adjacent tracked hash ledger pins
them and the implementation sources. The original capture selections and hashes
remain those in `DX11_NATIVE_PIXEL_MERGER_JOINS.md` and the raw reports.

Original fragment qualification, early-depth behavior, original scheduling and
complete public PS sessions remain open. These measured late tests provide inputs
for that work; they do not satisfy full Pixel History or P7/P8 removal acceptance.
