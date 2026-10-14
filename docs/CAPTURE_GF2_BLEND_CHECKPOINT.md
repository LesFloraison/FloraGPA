# GF2 native pipeline and scoped blend experiment

The formal six-frame gate remains failed. Native pipeline bindings, fifteen
checked input resources and the selected pixel's depth/stencil checkpoints
agree across ten complete replays, while the two known color outcomes recur.
Ten additional, deliberately modified executions disable blending on RTV0 for
Draw 5651 only. Those executions agree with each other, but are not acceptance
results and do not establish a blend hardware defect.

## Complete-replay observations

The retained frame is `frame-d3b245ec-09d2-4888-91af-5032f19ddbd6.gpa_frame`,
SHA256 `9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`.
All twenty processes select Intel vendor 32902 and report Intel Iris Xe. The
selected resource is 411, layer 0, sample 1, texel (1596, 1519). Each process
executes the complete frame, retains before/after GPU copies around Draw 5651,
then reads the copies and saves a lossless final PNG. No Map, Flush or explicit
wait occurs at that Draw boundary; observation still changes scheduling.

`pipeline` mode adds actual native shader, input-layout, topology, VS/PS sampler,
rasterizer, viewport, scissor, blend and RTV bindings to the preceding
input/color/depth observations. Descriptors, float bit patterns, object IDs,
view subresources, blend factors and sample mask are compared. Unsupported
shader class instances and RTV shapes fail explicitly. This does not claim to
inspect every possible D3D11 input or hidden driver state.

The native bindings agree before/after the Draw and across both experiment
groups. RTV0 uses blend state 361: source alpha and inverse source alpha with
ADD for both RGB and alpha, write mask 15. Independent blending is enabled,
alpha-to-coverage and logic operations are disabled, and sample mask is
`0xffffffff`. VS/PS remain 888/889; HS/DS/GS are absent. RTV resources are
411, 415 and 621. The baseline preserves the complete observed descriptors.

| Mode / outcome | Runs | Selected post-Draw raw half words | Final RGBA SHA256 |
| --- | ---: | --- | --- |
| Pipeline, capture-matching | 1 | 9127, 9247, 8891, 15143 | `d5ffe329528ecabc09517514d1b41c5a8d305210480ad9454c732051a697d0e7` |
| Pipeline, known alternate | 9 | 9109, 9233, 8865, 15151 | `b548683168d47d12344c2a976155eb716c4a14d135dcbb8c9df542b0abd0caa1` |
| Unblended RTV0, modified execution | 10 | 9246, 9321, 9012, 15124 | `ed5fef7cf3d18ea16c8647f85cc0f11a9f19671ac0354156f4cf1ec9f0c10c09` |

All twenty runs have the same selected pre-Draw color words, fifteen input
resource hashes/bindings, native pipeline and before/after depth/stencil samples.
Independent Pillow decoding verifies every saved PNG against its reported RGBA
hash. The ordinary alternate remains the same three differing pixels/channels
reported by earlier stages. The counterfactual images intentionally differ
more widely; they are retained separately and cannot replace captured images.

## Scoped override and independent tests

`unblended` mode requires a VS/PS DrawIndexed without SO or predication, with
the selected resource at RTV0 and an explicit independent blend state. It
copies the blend descriptor, changes only RTV0's BlendEnable to false, and
applies it immediately before the Draw. It restores the original COM binding,
blend factors and sample mask before the after-Draw observations and all later
commands. RAII restoration also covers exceptional exits. Reports explicitly
set `modified_draw: true` and always deny capture acceptance. Logic operations,
default/shared blend states and nested overrides fail explicitly.

```
cmake --build --preset capture-full-release --target FloraReplayTexelProbe FloraBlendCheckpointTests
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 pipeline 5651 5651 7
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 unblended 5651 5651 7
FloraBlendCheckpointTests NEW_REPORT_JSON
```

The excluded native test uses independent CPU expectations on WARP and Intel
debug devices, with 2x and 4x MSAA. Two render targets, a nontrivial sample mask,
and three phases (original blend, scoped override, restored blend) verify that
only RTV0 blending changes, other target blending remains effective, masked
samples retain their initial values, and native bindings are restored. Four
cases pass **72 exact texel checks**, **16 refusal checks**, with zero debug
warnings/errors. The existing texel validator passes **28 cases and 18
refusals**; source-boundary CTest and capture-only configuration also pass.
All GPU checks run serially.

No production Capture, Format or Replay implementation changes in this stage.
The ordinary replay executable retains SHA256
`215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.
Both the observer and scoped override remain development tools behind excluded
build targets. No comparison tolerance or formal-gate input changes.

## Interpretation and limits

Stable last stored color without blending is insufficient to infer stable
full-precision fragment outputs or identical fragment histories. Several
fragments may contribute under blending, while the unblended target retains
only the final write. Half-float storage quantizes the shader result, and a
blend-state change can alter driver execution. The earlier
[half-source experiment](CAPTURE_GF2_HALF_SOURCE.md) also cautions against
inverting a blend equation from already-quantized half-float output.

The next question is which fragments and full-precision outputs contribute to
the selected sample during this Draw. The current evidence does not assign
fault to the capture runtime, replay, shader, or driver, and does not explain
the additional output hash retained by the earlier color-only experiment.

Raw outputs and frozen binaries are preserved in
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-blend-checkpoint-20261014`.
The [baseline](capture-gf2-blend-checkpoint-baseline.json) records every run,
source/binary hashes, test counts and the evidence manifest. Legacy migration
parity is unchanged.
