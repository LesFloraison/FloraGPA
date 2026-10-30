# Native depth-only return accumulation

## Implemented behavior

`replayNativeDepthReturnCount` extends the existing private depth transport with
full-precision RGB unit accumulation. The report uses
`native-depth-return-count/1`. The original DSV and its depth/stencil operations
remain bound on private resources. A returning shader writes one into each RGB
component, with additive blending and a zero initial transport. The selected
pixel/layer/sample is read directly; neither pipeline nor occlusion queries are
converted into this count. Raw queries keep their own whole-output domain.

The native history CLI/Worker now handles depth-only targets with `--return-count`.
`native-pixel-draw-history/3` and `dx11-native/14` invalidate older result contexts.
Native debug sessions accept the new history contract. Existing color counts,
including the integer RT0 auxiliary route, retain their backend and behavior.

The helper adds a color output to the original depth-only PS body, preserving
its depth and coverage exports and discard behavior. For a draw without a PS it
uses an owned empty shader and explicitly records `original_null_pixel_shader`.
This is diagnostic return accumulation, not a count of original PS invocations,
an original fragment schedule, instance attribution or complete Pixel History.
Original eligibility remains null. The float exact-integer limit and unknown
results use the existing decoder. Alpha words do not participate in counting.

Depth selection is relative to the bound DSV. Reports now retain the DSV format,
source mip and first array layer. Worker requests translate the absolute GPA
resource layer into that view domain. Cancellation and the shared history
deadline cover the additional draw; transport and retained reports remain
subject to the existing payload budgets.

The depth-only route requires a 2D DSV (including array/MSAA views), no bound
RTVs, no occupied OM UAV slot zero, no original color export and disabled
alpha-to-coverage. Forced sampling is rejected. Depth selections with color
outputs bound remain explicitly not applicable in the history API. Other
unsupported helper configurations fail explicitly, without a successful result
or fallback. These boundaries remain work for full P5 parity.

## Evidence

Five Release suites pass **638 checks**, with zero failures or skips:

| Suite | Passed |
| --- | ---: |
| DXBC pixel completion | 285 |
| Native color return count | 189 |
| Isolated draw history jobs | 53 |
| Native analysis | 61 |
| Native debug session | 50 |

The completion matrix contains 117 depth-count observations on WARP, NVIDIA and
Intel. It covers single-sample storage and selected samples zero/three of 4x MSAA;
depth failure, stencil failure, shader depth, coverage, ordinary/early discard,
null PS, repeated coverage and repeated conditional discard. Three overlapping
triangles count as three, and discarding the middle primitive counts as two.
Repeated stencil-pass observations independently compare the count with the
stencil increment from an ordinary unmodified GPU draw. Every diagnostic draw's
depth bytes match the ordinary reference, including early-discard writes.
The original depth resource is restored. Cancellation raised after the draw,
payload failure and subsequent successful retry are also checked.

The standalone validator checks the complete 117-case matrix, prepared shader
binary hashes, original-draw context, actual adapter, transport, raw RGB values,
count validity and independent storage references. It rejects 702 corrupted
reports. Original shader and prepared bytecode files are retained; generated
count shader identity is recorded in each report and tied to the implementation.

The Worker matrix has 48 owned jobs, including nine added depth jobs: ordinary
depth-only, null PS, and nonzero mip/first array layer on all three adapters.
The original boundary cases retain null results. Nine GF2/BF1/Helldivers CLI
controls retain their prior independently observed color counts of one/two/one.
Artifact/context/module validation passes and rejects 288 corruptions. All 57
completed job module snapshots exclude RenderDoc and original GPA private
modules. This is completion-time evidence, not a full process-lifetime P8 audit.

GUI, CLI, Worker and the five test targets build successfully. The first build
and every GPU process in this increment passed; no golden or skip condition was
relaxed. GPU work ran serially. The real jobs take 284–1669 ms of Worker analysis
and report peak working sets from 371937280 to 2743435264 bytes. These include
whole-replay overhead, concern color controls, and are not isolated depth-count
performance measurements or P0/P8 acceptance.

## Reproduction

```text
cmake --build build/vs2022 --config Release --target FloraDxbcPixelCompletionTests FloraNativePixelReturnCountTests FloraNativeDrawHistoryJobsTests FloraNativeAnalysisTests FloraNativeDebugSessionTests FloraGPA FloraGPA.Cli FloraGPA.Worker --parallel 4
python tools/run_depth_return_count.py --binaries build/vs2022/Release --qt-bin <Qt/bin> --captures <capture-directory> --out <new-directory>
python tools/validate_depth_return_count.py --root <new-directory>/depth --out <depth-verification.json>
python tools/validate_history_return_count.py --root <new-directory>/jobs --prior-counts artifacts/dx11-native/pixel-count-auxiliary/regression/real --out <jobs-verification.json>
```

Raw evidence is retained under `artifacts/dx11-native/depth-return-count` outside
Git. The tracked `dx11-native-depth-return-count-evidence.json` pins sources,
binaries, observations and validation results. GPA/RenderDoc were not invoked
for these tests. The prior independent native count reports remain development
comparison inputs; they do not require either tool at runtime.

Full original fragment qualification, complete debugger parity, P1 engine
closure, RenderDoc removal and independent standalone release acceptance remain
open. This increment closes depth-only counting cases, not the full migration.

## Evidence integrity

Implementation commit `549568d1cfc091356d0b23cd09f5b2cc6e1253e6` pins the change. A separate read verified all 1,265 artifact hashes and sizes, 20 source Git blobs and hashes, and nine prior native reference hashes. All 57 completed jobs identify the copied Worker executable hash. The author and committer match the requested identity, and `main` remains at the frozen baseline.
