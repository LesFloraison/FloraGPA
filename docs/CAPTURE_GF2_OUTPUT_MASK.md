# GF2 counter-output write-mask control

The formal six-frame gate remains failed. The diagnostic count shader produces
the capture-matching image even when writes to its private counter target are
masked off. The original shader still produces both known outcomes in the same
interleaved experiment. Actual writes to the additional target are therefore
not necessary for the observed stability. This is an instrumentation effect,
not an accepted capture correction or proof of a driver defect.

## Same-bytecode comparison

The retained frame remains `frame-d3b245ec-09d2-4888-91af-5032f19ddbd6.gpa_frame`,
SHA256 `9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`.
All thirty processes select Intel vendor 32902 and report Intel Iris Xe. They
execute the complete frame, observing Draw 5651, resource 411, layer 0 and pixel
(1596,1519). Each saves a lossless final PNG and the preceding input/color/depth
and native-pipeline observations. GPU executions are serial.

`fragment-masked` uses the exact count-only bytecode and private RTV3 allocation
from the preceding stage. Its only requested blend-descriptor difference is
RTV3's RenderTargetWriteMask, changed from 15 to zero. Original RTV0/1/2,
their blend descriptors, depth/stencil state and sample mask are preserved.
The probe now reads the actually bound private counter blend descriptor and
records it. Both modes use ONE/ONE ADD for RGB and alpha. The shader's SHA256 is
`36e81f84589bff2faedefa6aaf92befc50d62cbb1532b6e02bba69e20f0f5199` in all
twenty transformed executions; neither the bytecode nor its output signature
changes between the two groups.

Ten iterations alternate masked output, enabled output and the original
`pipeline-debug` baseline, all using one frozen executable:

| Group | Runs | Capture-matching | Known alternate | Private target at both selected samples |
| --- | ---: | ---: | ---: | --- |
| Count shader, write mask 0 | 10 | 10 | 0 | Four float32 zeros |
| Count shader, write mask 15 | 10 | 10 | 0 | Four float32 ones |
| Original shader | 10 | 2 | 8 | No private target |

Matching final RGBA SHA256 is
`d5ffe329528ecabc09517514d1b41c5a8d305210480ad9454c732051a697d0e7`;
alternate SHA256 is
`b548683168d47d12344c2a976155eb716c4a14d135dcbb8c9df542b0abd0caa1`.
Independent PNG decoding verifies these hashes and the alternate's same three
differing pixels/channels. All thirty runs have identical inspected native
pipeline bindings, fifteen input-resource byte hashes/bindings, and the selected
before/after depth/stencil values. Comparison thresholds remain unchanged.

Both transformed Draw scopes have zero debug messages. Their 608 prefix
messages remain separate, and are not reclassified as a clean complete replay.
A zero masked counter does not mean that no fragments survived: it shows that
the output write mask suppressed storage. The unmasked count applies only to
the modified shader execution.

## Validation and limits

```
cmake --build --preset capture-full-release --target FloraReplayTexelProbe FloraFragmentCheckpointTests
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 fragment-masked 5651 5651 9
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 fragment-count 5651 5651 9
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 pipeline-debug 5651 5651 7
```

The independent native test adds the masked-counter case on WARP and Intel,
with 2x/4x MSAA. It checks the bound write mask, original three-target contents,
private counter values, scoped shader restoration and subsequent ordinary draws
against CPU expectations. All **20 cases, 624 exact texel checks and 120 refusal
checks** pass with zero debug warnings/errors. Original render targets retain
their expected blend results while the masked private target stays zero.

The texel validator passes 28 cases and 23 refusals, including an insufficient
masked-mode read budget. Source-boundary CTest and capture-only configuration
pass. No production Capture, Format or Replay implementation changes. The
ordinary replay executable retains SHA256
`215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.
The source remains restricted to excluded development tools.

These experiments cannot identify the original shader's full-precision inputs
or execution path. Even masked output declarations/instructions can affect a
different shader's compilation or execution. No masked-counter path is proposed
as a capture fix, and no formal frame becomes accepted.

Before attributing the original variation to shader compilation, the remaining
unobserved inputs must be checked. In particular, only one pixel's depth/stencil
samples have been compared so far; the complete depth planes and neighboring
fragments are not proven equal. The next input comparison should cover those
planes during unmodified complete replay, rather than infer full equality from
the selected texel. The additional hash from the earlier color-only experiment
also remains unresolved.

Raw outputs and frozen binaries are retained under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-output-mask-20261014`.
The [baseline](capture-gf2-output-mask-baseline.json) records all thirty runs,
source/binary identities and the evidence manifest. Legacy migration parity
and the failed six-frame gate remain unchanged.
