# GF2 texel variation at DrawIndexed 5651

The six-frame acceptance gate remains failed. This stage localizes the retained
variable file's observed MSAA texel variation to the execution interval of
DrawIndexed **5651**: in ten complete replays, the selected texel agrees before
this Draw, but its two observed values immediately afterward reproduce with
the two known final images. One run also has an additional final-image hash.
This does not establish equality of all Draw inputs,
identify the first divergent operation anywhere in the frame, or supply a fix.

## Retained input and observations

The input remains `frame-d3b245ec-09d2-4888-91af-5032f19ddbd6.gpa_frame`, SHA256
`9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`.
All executions explicitly select Intel vendor 32902 and report Intel Iris Xe.
The selected storage is resource **411**, layer **0**, sample **1**, texel
**(1596, 1519)**, a 3456 x 1944 RGBA16 texture with two samples.

The preceding paired-plane experiments found that this texel differed between
two complete outputs. This stage uses three complementary observations:

| Experiment | Executions | Result |
| --- | ---: | --- |
| Before/after reads around the last 20 candidate RTV writers | 10 | All produce the alternate final image; only events 5628, 5641 and 5651 change the selected texel in this range |
| Prefix ending before/after 5641 and 5651 | 24 | Six runs of each boundary agree; no matching-output variant reproduced |
| Full replay followed only by final image/texel reads | 10 | Two capture-matching final hashes, eight alternate hashes |
| Full replay with GPU copies immediately before/after 5651, read after replay | 10 | One capture-matching hash, eight known alternate hashes, one additional hash; all ten pre-Draw texels agree |

The GPU-copy experiment does not insert a CPU readback, Flush or explicit wait
at the Draw boundary. It still adds allocations and CopyResource commands and
changes execution timing. Both known outputs also occur without boundary
observation, but the additional output reinforces the limit on causal claims.

The copied pre-Draw texel is always `[7759, 8093, 7731, 15285]`. Copied post-Draw
values and the selected texel after full replay agree within every execution:

| Outcome | Raw binary16 component words | Final RGBA SHA256 |
| --- | --- | --- |
| Capture-matching | 9127, 9247, 8891, 15143 | `d5ffe329528ecabc09517514d1b41c5a8d305210480ad9454c732051a697d0e7` |
| Alternate | 9109, 9233, 8865, 15151 | `b548683168d47d12344c2a976155eb716c4a14d135dcbb8c9df542b0abd0caa1` |

These hashes are checked against the preceding stage's retained full RGBA
outputs, where the alternate has three differing pixels/channels. This stage
saves hashes and texel words instead of duplicating those large outputs. No
new capture, substitute acceptance file or tolerance is introduced.

Checkpoint run 2 has the alternate texel words, but final-image SHA256
`f8ec54f9d7875fcc3d9e3ae272b8b657900f59b168a076899a213b9c42376f07` differs from both
known images. Its full pixels were not saved by this compact probe, so neither
their location nor difference count is established. This run is retained as an
additional mismatch, not counted as the known three-pixel alternate. One texel
is insufficient to characterize the complete output or all effects of copying.

## Candidate Draw and remaining uncertainty

Event 5651 is `DrawIndexed(7401, 0, 0)`, using VS 888 and PS 889, RTVs 412/416/622
and DSV 420. The recorded state uses blend 361, rasterizer 91, depth/stencil
state 496, stencil reference 200 and sample mask `0xffffffff`; it has no bound
predicate or stream output. The preserved candidate-state export also includes
the preceding two modifying Draws and shader bindings.

The VS SHA256 is
`18669b668cb39b907a02cfec253f74b24d0c09c6ae73d0d65f27455032b2d869`, and PS SHA256
is `fe92913b679084eb0c88a589285f8470cef44e70075b02a966adad683fd7bd95`. These exactly
match the earlier, separate [half-source experiment](CAPTURE_GF2_HALF_SOURCE.md).
The resource IDs, target texel, Draw arguments and capture are different. Shader
identity connects the investigations; it does not transfer the earlier input
stability observations to this new frame.

Only one pre-Draw texel is proven equal here. Other color/depth samples, shader
resources, buffer bytes and shader execution results have not been compared
between the two outcomes in these same executions. The next step is to retain
those Draw inputs without replacing the complete replay with a prefix, then
separate input variation from shader/rasterization/blending variation. Neither
the driver nor a capture component is assigned fault on the current evidence.

## Development probe and verification

`FloraReplayTexelProbe` is an explicit, excluded development target linking only
Replay. It introduces no Capture dependency, production Replay changes or
runtime packaging changes. Build and invoke it with:

```
cmake --build --preset capture-full-release --target FloraReplayTexelProbe
FloraReplayTexelProbe FRAME NEW_OUT VENDOR RESOURCE SAMPLE LAYER X Y MODE FIRST LAST READ_BUDGET
```

- `before` / `after` stop at one Draw/Dispatch boundary; FIRST must equal LAST.
- `trace` completes the frame and reads before/after candidate RTV writers in
  the inclusive event range. It does not inventory Copy/Clear/UAV mutations.
- `final` completes the frame without a boundary observer; FIRST/LAST remain
  validated event IDs but do not truncate execution.
- `checkpoint` copies the selected resource before/after one RTV-writing Draw,
  completes replay, and reads the retained copies through integer SRVs into a
  one-pixel RGBA32_UINT target. It rejects bound predication. FIRST must equal
  LAST and the budget must allow two checkpoint reads and one final read.

Selection is limited to RGBA8/RGBA16, 2x/4x MSAA and one mip, with checked
sample/layer/texel bounds, at most 1024 observations, a 2 GiB selected-plane
budget and a separate 2 GiB bound on both MSAA checkpoint copies. Existing
destinations are refused. Reports begin incomplete and remain incomplete on
failure; every report explicitly denies acceptance. Full modes verify that
output is an executed GPU texture and that the final sample read leaves output
unchanged. GPU copies and reads never supply reference pixels to replay.

`tools/validate_capture_texel_probe.py` passes **28 cases** and **16 refusals**
against the independent native RGBA8 CPU oracle. Cases cover two startup paths,
2x/4x samples, both array layers, corner/edge coordinates, before/after state,
full-frame checkpoints, and deleted initialization/application Draw controls.
The same frozen checkpoint binary performs the ten GF2 boundary-copy runs.
The RGBA16 observations agree with the preceding full-plane reader values;
this stage does not add an independent native RGBA16 CPU fixture.

Four relevant CTests pass, as do serial GF2/BF1 golden outputs and both
Draw-suppression controls. Capture-only configuration and source boundary
checks pass. The ordinary replay executable retains SHA256
`215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.

Raw reports, three frozen probe revisions, shader exports, validation and
manifest are retained in
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-texel-boundary-20261014`.
The initial trace/prefix revision and the intermediate final-mode revision have
separate hashes; neither is retroactively described as the final checkpoint
binary. The [baseline](capture-gf2-texel-boundary-baseline.json) records these
identities, experiment groups and checks. Legacy migration parity is unchanged.
