# GF2 complete depth/stencil plane comparison

The formal six-frame gate remains failed. Complete shader-readable depth/stencil
planes agree across ten unmodified complete replays, including three capture-
matching and seven alternate color outcomes. Two further runs reproduce the
same four plane hashes and confirm that the depth values are finite, positive
and normal. This closes the earlier single-pixel limitation for depth/stencil
contents; it does not establish identical shader execution or provide a fix.

## Full-plane evidence

The retained frame is `frame-d3b245ec-09d2-4888-91af-5032f19ddbd6.gpa_frame`,
SHA256 `9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`.
Every run selects Intel vendor 32902 and reports Intel Iris Xe. Draw 5651's DSV
uses resource 419, a single-layer 3456 x 1944, two-sample R32G8X24 typeless
texture viewed as D32_FLOAT_S8X24_UINT. Shader, render targets, blend state,
original command stream and comparison thresholds are unchanged.

The existing before/after GPU depth copies are retained at the Draw boundary.
The new reader runs only after the entire frame has executed. It loads depth
through R32_FLOAT_X8X24_TYPELESS and stencil through X32_TYPELESS_G8X24_UINT,
then writes `(asuint(depth), stencil)` to a private R32G32_UINT output. Mapped
rows are packed into eight bytes per pixel: little-endian float32 depth bits,
then a uint32 containing the stencil byte. Undefined 24-bit source padding,
GPU compression metadata and staging row padding are excluded. This is a
comparison of shader-readable contents, not opaque hardware storage.

Each plane contains **6,718,464 pixels / 53,747,712 canonical bytes**. Four
planes cover two samples before and after the Draw, totaling 214,990,848 bytes
per execution. Hashes agree across all thirteen retained runs, including one
exploratory run, the ten validated runs and two final statistics runs:

| Boundary | Sample | Canonical plane SHA256 |
| --- | ---: | --- |
| Before | 0 | `1fa71f428c1b66f50e125289c36b6041e98e90ba0f3d74bdc2edb8d9efb3fa32` |
| Before | 1 | `2f4a4e7fa02cd5fd93743bfc8c440ac8dbd83db4e1774339d94dd030ad68586d` |
| After | 0 | `48c1da4fdbbb8a322ca3b983a8e8083840b69404c1a7fef68904b387d16ff0ac` |
| After | 1 | `e8d3b0d05f5856eabb73e0b081700595c4478a88436b0131e6c8bb4c813532ab` |

The final two reports scan all canonical values. Each plane has zero nonfinite,
negative, subnormal and zero depth values. Stencil values are limited to
0, 200, 201, 203 and 204; the baseline records complete histograms. The selected
pixel agrees independently with the preceding single-texel reader in every
plane. These results do not depend on assigning meaning to unused padding bits
or NaN payloads.

The ten validated runs retain both known final RGBA hashes: three match
`d5ffe329528ecabc09517514d1b41c5a8d305210480ad9454c732051a697d0e7`, and seven
produce `b548683168d47d12344c2a976155eb716c4a14d135dcbb8c9df542b0abd0caa1`.
The exploratory and two statistics runs produce the latter. Independent PNG
decoding verifies every RGBA hash and the alternate's same three differing
pixels/channels. The fifteen checked input-resource hashes/bindings and observed
native pipeline remain identical as well. Full depth planes are hashed in memory
and not written as large raw dumps; all reports and final PNGs are retained.

## Independent verification

The native depth producer now writes different values at every pixel of a 5 x 3
image, separately for each layer and sample. Depth uses exact multiples of 1/64;
stencil also varies by position. The original is overwritten after retaining a
checkpoint, and both epochs are compared against independent little-endian CPU
expectations. WARP and Intel, typed/typeless source storage, two layers and 2x/4x
MSAA give **eight cases, 96 complete planes / 1,440 plane texels**, plus the
preceding **192 selected-texel checks**. All **64 refusals** pass and debug
queues contain zero warnings/errors. DSV, sample mask, depth/stencil state and
scissor bindings survive the readback. The odd width exercises row-padding
removal; the spatial pattern detects row/column or layer/sample mixups.

The plane reader limits each result to 128 MiB and rejects invalid formats,
sample/layer selections and insufficient budgets. The CLI retains its narrower
single-layer D32S8 scope and prior copy budgets. Its read budget covers three
color reads, two selected-depth reads per sample and two full-plane reads per
sample: 11 for 2x MSAA, 19 for 4x. Deferred readback commands restore immediate
context state. No new read, Map or wait is inserted at the selected Draw boundary.

```
cmake --build --preset capture-full-release --target FloraReplayTexelProbe FloraDepthCheckpointTests
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 depth-planes 5651 5651 11
FloraDepthCheckpointTests NEW_REPORT_JSON
```

An initial expanded native build failed because a test referenced an unavailable
append helper. A stale, pre-expansion test binary was inadvertently run; its
report is explicitly excluded and preserved with the old binary identity.
The corrected independent CPU encoder and new native test passed before the ten
validated GF2 runs. The earlier exploratory replay is separately identified.
Build repairs and exclusions are recorded in the raw `build-notes.json`.

The final texel validator passes 28 cases and 24 refusals. Source-boundary CTest
and capture-only configuration pass; GPU checks ran serially. No production
Capture, Format or Replay implementation changes. The ordinary replay binary
remains `215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.

## Remaining scope

Complete depth/stencil contents are now compared, but this does not reveal
intermediate fragment histories, post-vertex-shader values, hardware execution
decisions, or the original pixel shader's full-precision intermediates. Added
copies can still affect scheduling. The observed shader instrumentation effects
remain diagnostic and cannot substitute for the original execution. The formal
six-frame mismatch and the additional earlier output hash remain unresolved.

Raw evidence is preserved under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-depth-planes-20261014`.
The [baseline](capture-gf2-depth-planes-baseline.json) records binary groups,
plane hashes/statistics, test coverage, excluded results and the evidence
manifest. Legacy migration parity is unchanged.
