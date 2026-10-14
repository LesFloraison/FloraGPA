# GF2 count-only output and shader recreation controls

The formal six-frame gate remains failed. Count-only instrumentation retains
every original color instruction and adds just an output declaration and a
constant write. Twenty instrumented complete replays produce the capture-
matching image, with one surviving write at each selected MSAA sample. Original
bytecode still varies when recreated immediately before the Draw or when an
extra private RTV is bound without adding the shader output. This narrows the
effect to the modified execution; it does not establish a production fix.

## Controls

The retained frame is `frame-d3b245ec-09d2-4888-91af-5032f19ddbd6.gpa_frame`,
SHA256 `9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`.
All runs select Intel vendor 32902 and report Intel Iris Xe. Draw 5651, color
resource 411, layer 0 and pixel (1596,1519) remain unchanged. All groups execute
the complete frame and save lossless final PNGs. They compare the previously
checked native pipeline, fifteen input resources, selected color checkpoints,
and both depth/stencil samples. GPU work is serial.

The new diagnostic modes are:

* `fragment-count`: add `dcl_output o3.xyzw` and a constant-one output before
  the final return. Bind a cleared private RGBA32_FLOAT MSAA RTV3 with additive
  ONE/ONE blending. Preserve original RTV0/1/2 and their blend descriptors.
  No private color mirror is allocated and no original arithmetic is duplicated.
* `fragment-binding`: use the original shader bytes with the same private RTV3
  allocation, clear and binding. No output-three instruction is added, so the
  private target remains zero. This is an attachment control, not a count of
  original fragments.
* `shader-recreate`: create a PS from the exact original bytecode immediately
  before the Draw, bind it for that Draw, and restore the original PS afterward.
  No RTV or blend override is applied. The replacement COM identity is distinct
  in every execution.
* `shader-repack`: retain ISGN/OSGN/SHDR and rebuild the container before the
  same scoped recreation. Inspection shows that this particular source already
  contains only those three chunks in canonical order: the resulting bytes and
  SHA256 are **identical**. These ten executions are therefore another original-
  bytecode recreation group, not an independent metadata-removal experiment.

Original PS SHA256 is
`fe92913b679084eb0c88a589285f8470cef44e70075b02a966adad683fd7bd95`;
the count-only PS is
`36e81f84589bff2faedefa6aaf92befc50d62cbb1532b6e02bba69e20f0f5199`.
CPU verification removes precisely the two added instruction rows and compares
every original executable word exactly. The transformer still accepts only the
pinned GF2 shader. Native D3D11 shader creation and scoped debug checks pass.

The first experiment interleaves four groups, ten times each. A second uses a
single updated binary and alternates binding-only and count-only ten times.

| Group | Executions | Capture-matching | Known alternate |
| --- | ---: | ---: | ---: |
| Count-only, first batch | 10 | 10 | 0 |
| Original PS recreated | 10 | 3 | 7 |
| Identical-byte repack/recreation | 10 | 0 | 10 |
| Original PS, debug baseline | 10 | 3 | 7 |
| Binding-only, paired batch | 10 | 1 | 9 |
| Count-only, paired batch | 10 | 10 | 0 |

One earlier exploratory count-only execution also matched and is retained
separately. All 61 runs are accounted for; no alternate result is discarded.
Matching RGBA SHA256 is
`d5ffe329528ecabc09517514d1b41c5a8d305210480ad9454c732051a697d0e7`;
the known alternate is
`b548683168d47d12344c2a976155eb716c4a14d135dcbb8c9df542b0abd0caa1`.
The alternate retains the same three differing pixels/channels. Independent
PNG decoding verifies every recorded RGBA hash.

Count-only runs store float32 one (`0x3f800000`) in all four counter channels
at both selected samples. Binding-only runs store zero. All inspected original
input-resource bytes/bindings, native pipeline and selected depth/stencil
checkpoints agree across these executions. Added work still changes scheduling,
and the counts do not establish original uninstrumented fragment histories.
All observed replacement/attachment Draw scopes have zero debug messages; the
608 prefix messages remain separate and are not reclassified as a clean replay.

## Validation and boundaries

The private attachment helper now rejects any bound OM UAV before changing
bindings, since it does not preserve UAV counters. Both original and extended
slot limits follow the device feature level. Native refusal tests bind a real
UAV and verify this rejection. Shader replacements use scoped restoration and
reject nested overrides and class instances.

The independent native test now covers WARP and Intel, 2x/4x MSAA, float/half
mirrors, count-only and binding-only modes: **16 cases, 504 exact texel checks,
96 refusals**, zero debug warnings/errors. Three original RTVs verify that
the unused RTV2 contents survive both replacement and binding-only modes.
Two overlapping draws count two where enabled; binding-only and masked samples
stay zero. Original RTV0/1 blend results, later ordinary draws, and native
binding restoration agree with CPU expectations. A scoped identical-bytecode
PS replacement also restores the original shader. Earlier test binaries and
their narrower results remain retained under distinct names.

```
cmake --build --preset capture-full-release --target FloraReplayTexelProbe FloraFragmentCheckpointTests
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 fragment-count 5651 5651 9
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 fragment-binding 5651 5651 9
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 shader-recreate 5651 5651 7
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 shader-repack 5651 5651 7
```

The final texel validator passes 28 cases and 22 refusals. Source-boundary CTest
and capture-only configuration pass. Production Capture, Format and Replay
implementations are unchanged; the ordinary replay executable remains
`215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.
All new behavior remains in excluded development tools. No shader substitution
is applied to accepted captures, and no comparison tolerance changes.

## Interpretation and retained evidence

Fresh PS identity and private attachment binding each retain the alternate
outcome. Neither is sufficient to explain the observed count-only stability.
The added shader output remains associated with a changed execution result,
even without duplicating color arithmetic. This does not identify which driver
compilation, precision or execution decision changed, nor prove equal original
fragment counts. No component is assigned fault. The next investigation must
isolate the extra output's execution effect without adopting the instrumentation
as a capture correction. The formal six-frame mismatch and the additional hash
from the earlier color-only experiment remain unresolved.

Raw evidence, distinct frozen binaries and a source snapshot are retained in
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-count-checkpoint-20261014`.
The [baseline](capture-gf2-count-checkpoint-baseline.json) maps each execution
group to its binary and records the evidence manifest. Legacy migration parity
is unchanged.

To recover evidence-storage headroom, one existing task-generated 1,352,555,110-
byte capture was NTFS-compressed in place. Its before/after SHA256 is identical;
the compression log and identity are retained. No capture or evidence was
deleted, and the old workspace remains intact.
