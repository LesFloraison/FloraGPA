# GF2 fragment outputs during complete replay

The formal six-frame gate remains failed. This stage observes the selected
sample while retaining original RTV0 blending. A diagnostic shader mirrors
output zero to a private target and writes a constant to an additive counter
target. In the instrumented executions, each selected MSAA sample receives one
surviving write. Both half- and full-precision mirror targets produce the
capture-matching final image consistently. The original shader still produces
both known outcomes with the same debug-layer setting. The observation changes
execution and is not a production correction.

## Experiment and scope

The input remains `frame-d3b245ec-09d2-4888-91af-5032f19ddbd6.gpa_frame`, SHA256
`9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`.
DrawIndexed 5651 uses PS889, whose bytecode SHA256 is
`fe92913b679084eb0c88a589285f8470cef44e70075b02a966adad683fd7bd95`.
The observation remains resource 411, layer 0, pixel (1596,1519), on Intel Iris
Xe (vendor 32902). All processes execute the complete frame, save final PNGs,
and compare the native pipeline, fifteen input resources, selected color and
both depth/stencil samples as in the preceding checkpoint stages.

The transformer accepts only this exact, verified shader hash. It preserves
every original executable instruction word, adds two output declarations,
duplicates the original `mad o0.xyz` and `mov o0.w` into output two, and writes
four float ones to output three before the single final return. A compiled
four-target signature replaces OSGN; stale reflection/statistics chunks are
omitted. CPU verification removes precisely those five added rows and confirms
the original instruction stream byte for byte. The mirrored DXBC SHA256 is
`ba7fb58c74c2496ec7fffe800640ae3c8c0ce41b13c7142d1342645e827c80d4`.

RTV0 and RTV1 retain their original resources, formats and blend descriptors.
A private RGBA32_FLOAT or RGBA16_FLOAT MSAA RTV replaces RTV2 for this Draw;
the original shader has no output two. Private RTV3 is RGBA32_FLOAT with
ONE/ONE additive blending. Both private targets start at zero. The selected
Draw retains its native depth/stencil state and sample mask. Immediately after
the Draw, original PS, RTVs, DSV, blend object, factors and mask are restored.
Private targets are read only after complete replay. Added work and output
bindings can still change scheduling and driver compilation.

The tool rejects an unknown source shader, unsupported shape, excessive private
allocation, bound predication/classes, missing explicit independent blending,
alpha-to-coverage, logic operations, occupied RTV3..7, or reuse of an observation.
The CLI additionally requires VS/PS DrawIndexed without HS/DS/GS/SO/predication.
These restrictions are diagnostic scope, not additions to supported capture.

## Results

The main paired experiment alternates two modes for ten iterations, using one
frozen binary and identical transformed bytecode. Original-shader controls
enable the debug layer without transforming the shader. The final binary also
repeats each instrumented mode once after adding that control CLI mode.

| Group | Executions | Capture-matching final RGBA | Known alternate |
| --- | ---: | ---: | ---: |
| Full-precision mirror | 10 | 10 | 0 |
| Half-precision mirror | 10 | 10 | 0 |
| Original shader, debug layer | 10 | 1 | 9 |
| Final-binary mirror checks | 2 | 2 | 0 |

The earlier exploratory execution and ten full-precision runs before adding
the half-precision control are preserved separately; all eleven also matched.
They are not counted as the main paired experiment. No failed or alternate
run is discarded.

All instrumented runs have the following observations. The counter's four
channels each hold float32 one (`0x3f800000`) at each sample.

| Sample | Mirrored float32 words | Mirrored half words |
| --- | --- | --- |
| 0 | 1000506070, 1001931390, 999076797, 1065353216 | 7444, 7618, 7269, 15360 |
| 1 | 1015270884, 1015884030, 1013355746, 1063427123 | 9246, 9321, 9012, 15124 |

For sample 1, the mirrored floats are approximately
`[0.0160893872, 0.0172314607, 0.0140735824, 0.885195911]`. These are values in
the modified execution, not a measurement of the original shader's internal
blend input. The half words agree with the preceding unblended experiment.

Every run's input-resource bindings/hashes, observed native pipeline and selected
before/after depth/stencil values agree with the preceding stage. Normal debug
controls retain the same alternate three differing pixels/channels. Independent
PNG decoding verifies each reported RGBA hash; comparison rules are unchanged.
Instrumented scopes have zero debug messages. The 608 prefix messages remain
separate and are not relabeled as a clean full replay.

## Verification and boundaries

```
cmake --build --preset capture-full-release --target FloraReplayTexelProbe FloraFragmentCheckpointTests
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 fragment 5651 5651 11
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 fragment-half 5651 5651 11
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 pipeline-debug 5651 5651 7
FloraFragmentCheckpointTests NEW_REPORT_JSON
```

The excluded native test uses independently compiled constant shaders and CPU
expectations. WARP/Intel, 2x/4x MSAA and both mirror formats give eight cases,
**192 exact texel checks**, **32 refusals**, zero debug warnings/errors. Two
overlapping draws produce count two on enabled samples, zero on masked samples,
the expected float/half mirror values, and the original blended RTV0/RTV1
results. Native bindings are restored before readback and before a later
ordinary draw. This verifies the observation machinery, not semantic equivalence
of the transformed GF2 shader.

The texel validator passes 28 cases and 20 refusals, including insufficient
fragment read budgets. Source-boundary CTest and capture-only configuration
pass. GPU work is serial. No production Capture, Format or Replay code changes;
the ordinary replay executable remains
`215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.

The two original-shader outcomes recur with the debug layer, so that setting
alone does not explain stability of the modified runs. Half-format observation
also matches, so the private mirror's float32 format is not necessary for the
observed stability. Neither result proves that original executions have the
same fragment count or full-precision outputs. Extra output instructions and
bindings remain confounded. The next useful control reduces this instrumentation
while retaining original arithmetic, rather than treating a modified shader as
a capture fix. The additional hash from the earlier color-only experiment also
remains unexplained.

Raw outputs, source snapshot and distinct frozen binaries are retained under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-fragment-checkpoint-20261014`.
The [baseline](capture-gf2-fragment-checkpoint-baseline.json) associates each
group with its binary and records source hashes and the evidence manifest.
Legacy migration parity is unchanged; no formal frame is newly accepted.
