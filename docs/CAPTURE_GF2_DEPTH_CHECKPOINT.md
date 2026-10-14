# GF2 depth/stencil checkpoints across the varying Draw

The formal six-frame gate remains failed. This stage compares the selected
pixel's two MSAA depth/stencil samples during complete replay. Their before-
and after-Draw values agree across ten executions, including both known color
outcomes. Together with the unchanged input-resource comparisons, this narrows
the investigation without proving that every input is equal or providing a fix.

## Same-execution evidence

The retained file is `frame-d3b245ec-09d2-4888-91af-5032f19ddbd6.gpa_frame`, SHA256
`9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`.
Every process selects Intel vendor 32902 and reports Intel Iris Xe. The Draw
is 5651; the observed color resource remains 411, layer 0, sample 1, texel
(1596, 1519). Its bound DSV is backed by resource **419**, a 3456 x 1944, two-
sample R32G8X24 typeless texture viewed as D32_FLOAT_S8X24_UINT.

The probe retains GPU copies of that bound depth resource immediately before
and after the Draw. As with the preceding color/input copies, no Map, Flush or
explicit wait occurs at the Draw boundary. The complete application replay
continues before copies are read. Added allocations and copies still alter
scheduling and are not claimed to be neutral observations.

The selected depth values change during the Draw, but the same change occurs
in all ten executions. Raw float32 bits and stencil values are:

| Sample | Before depth bits | After depth bits | Before/after stencil |
| --- | ---: | ---: | ---: |
| 0 | 1025609039 | 1025641657 | 200 / 200 |
| 1 | 1025614223 | 1025625546 | 200 / 200 |

The native depth/stencil binding also agrees before/after and across runs:
depth enabled, write mask 1, comparison 7, stencil enabled, read/write masks
255, reference 200, front-face operations `[1,1,3,8]` and back-face operations
`[1,1,1,8]`. Numeric values are retained directly from the native descriptors.
The previously checked fifteen VS/PS/IA input resources and their bindings
remain identical in these same executions.

Color outcomes still vary:

| Outcome | Runs | Selected post-Draw color words | Final RGBA SHA256 |
| --- | ---: | --- | --- |
| Capture-matching | 2 | 9127, 9247, 8891, 15143 | `d5ffe329528ecabc09517514d1b41c5a8d305210480ad9454c732051a697d0e7` |
| Known alternate | 8 | 9109, 9233, 8865, 15151 | `b548683168d47d12344c2a976155eb716c4a14d135dcbb8c9df542b0abd0caa1` |

Each run saves a lossless PNG and verifies its RGBA round trip. Independent
Pillow decoding confirms the hashes and the alternate's same three differing
pixels/channels. The source frame, comparison files and tolerance are unchanged.
This stage neither replaces a formal-gate capture nor explains the additional
output hash retained by the earlier color-only checkpoint experiment.

## Probe and tests

The existing excluded development probe adds `depth` mode:

```
cmake --build --preset capture-full-release --target FloraReplayTexelProbe FloraDepthCheckpointTests
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 depth 5651 5651 7
FloraDepthCheckpointTests NEW_REPORT_JSON
```

This mode retains the preceding input/color diagnostics and requires a bound,
single-layer D32S8 MSAA DSV matching the selected color dimensions and sample
count, plus an explicit depth/stencil state. Other DSV formats or shapes fail
explicitly. Both depth copies have an aggregate 512 MiB limit, in addition to
the preceding input/color budgets. The read budget must cover three color
observations plus two depth observations per sample (7 for 2x, 11 for 4x).

The depth reader uses separate depth and stencil SRVs, returning the float32
depth bits and 8-bit stencil in an integer output. The unused 24 padding bits
are not compared or described as meaningful captured data. Compatible source
families, typeless read storage and sample/layer/coordinate bounds are checked.
Reports remain incomplete on failure and always deny capture acceptance.

`FloraDepthCheckpointTests` independently renders per-layer/per-sample depth
and stencil patterns on WARP and Intel debug devices. It retains a checkpoint,
clears the original resource to different values, and reads both copies against
exact CPU expectations. Eight cases cover typed/typeless storage, 2x/4x MSAA,
two array layers and corner coordinates: **192 exact texel checks**, **40
refusals**, zero debug warnings/errors. DSV, sample-mask and depth-state bindings
remain intact after inspection. The CLI's narrower single-layer scope does not
expand merely because the underlying reader's array tests pass.

The texel validator passes 28 cases and 18 refusals, including too-small depth
read budgets and a missing DSV. The source-boundary CTest and capture-only
configuration pass. All GPU work ran serially. No production Capture, Format
or Replay implementation changed; the ordinary replay executable retains
SHA256 `215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.
All new code remains under development tools and excluded build targets.

## Limits and next question

These observations cover one pixel's two depth/stencil samples, not the full
depth image or every intermediate fragment's depth test. Equal final depth
does not establish equal fragment histories. Other color samples, native
sampler/rasterizer/blend state and fragment shader results are not all compared
here. No first-divergence claim for the whole frame is made.

The next investigation is the color/fragment path and blending around this
Draw while retaining complete replay. Neither a driver nor capture component
is assigned fault from the current evidence.

Raw outputs and frozen binaries are retained in
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-depth-checkpoint-20261014`.
The [baseline](capture-gf2-depth-checkpoint-baseline.json) records all ten runs,
binary/source identities, native test counts and the evidence manifest.
Legacy migration parity is unchanged.
