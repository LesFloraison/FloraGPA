# Comparing GF2 Draw inputs during complete replay

The formal six-frame gate remains failed. Ten complete executions of the
retained variable file reproduce both known final images while all **15 checked
input resources (52,511,484 bytes)** and their inspected bindings agree. This
narrows the investigation of DrawIndexed 5651; it does not establish equality
of every input or identify a production fix.

## Evidence

The frame remains `frame-d3b245ec-09d2-4888-91af-5032f19ddbd6.gpa_frame`, SHA256
`9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`.
Each process explicitly selects Intel vendor 32902. The selected output sample
is resource 411, layer 0, sample 1, texel (1596, 1519).

Immediately before Draw 5651, the development probe obtains the native VS/PS
constant-buffer and SRV bindings and IA vertex/index buffers. It deduplicates
their resource identities, validates supported shapes and a 512 MiB aggregate
budget, and enqueues copies to staging resources. It does not Map, Flush or
wait at this boundary. The complete application replay continues, including
later resource updates; only afterward are the retained copies read and hashed.
These extra copies and allocations still affect execution scheduling.

The checked set comprises:

- Nine buffers: 17, 60, 363, 365, 798, 799, 800, 826 and 896. These cover all
  bound VS/PS constant buffers and IA vertex/index buffers at this Draw.
- Six single-sample Texture2D resources: 97, 386, 422, 675, 676 and 690. These
  cover the bound VS/PS SRVs, including cube faces, mip chains, compressed
  textures and the sampled depth texture.

All ten runs have identical resource descriptors, content hashes, SRV
descriptors, CB ranges, IA strides/offsets and recorded MinLOD values. The
selected pre-Draw texel also remains `[7759, 8093, 7731, 15285]` in every run.
The two observed post-Draw values persist to the selected frame-end texel:

| Outcome | Runs | Post-Draw words | Final RGBA SHA256 |
| --- | ---: | --- | --- |
| Capture-matching | 2 | 9127, 9247, 8891, 15143 | `d5ffe329528ecabc09517514d1b41c5a8d305210480ad9454c732051a697d0e7` |
| Known alternate | 8 | 9109, 9233, 8865, 15151 | `b548683168d47d12344c2a976155eb716c4a14d135dcbb8c9df542b0abd0caa1` |

Every complete-mode probe now saves a lossless `frame.png`. The probe reloads
it and checks all RGBA bytes, and external Pillow decoding independently
checks its hash. The retained matching full RGBA from the preceding stage is
the comparison reference. The alternate still differs at the same three
pixels/channels. There is no additional final-image hash in these ten runs;
the additional hash from the preceding checkpoint experiment remains an
unresolved, separately retained observation.

## Verification and limitations

The new excluded `FloraInputCheckpointTests` uses independent native data
patterns on WARP and Intel with debug devices. It copies eight resources,
overwrites every original subresource before reading the copies, then verifies
both pre- and post-overwrite contents against the CPU oracle: **32 exact
resource-hash checks**. Coverage includes CB/VB/IB/structured buffers, RGBA8
arrays, a BC1 cube with small mips, R16 typeless data, arbitrary RGBA16 typeless
bits, finite RGBA16_FLOAT data, and a nonzero ResourceMinLOD. Ten refusal checks
cover repeated/missing checkpoints, missing identities, MSAA SRVs and 1D
textures. Both devices report zero debug warnings/errors.

The existing texel validator passes 28 cases and 16 refusals with the final
probe binary, now also checking saved PNG bytes against its independent CPU
oracle. The source-boundary CTest and capture-only configuration pass.
Production capture/replay source and ordinary replay executable are unchanged;
the latter retains SHA256
`215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.
No new golden replay claim is made for this stage: the prior stage's GF2/BF1
checks remain the relevant unchanged-production baseline.

An exploratory native test using arbitrary bit patterns in **typed**
RGBA16_FLOAT failed exact initial CPU bytes on Intel. A separate direct native
upload/copy/readback control, without the input-copy helper, changed 22 NaN
words to 0xfe00 on Intel and changed none on WARP. Its source, executable and
failure log remain preserved. This establishes behavior of that tested native
path, not whether upload or copying caused it, and not a cause of the GF2
difference. It is not counted as a passing arbitrary typed-float bit test.
The passing corpus separately covers arbitrary typeless bits and finite typed
floats; GF2 acceptance tolerances and files are unchanged.

Initial exploratory GF2 runs also used a probe that queried GetResourceMinLOD
without checking the RESOURCE_CLAMP creation flag. Debug validation caught
that invalid diagnostic call. The final helper queries only clamped resources
and records zero otherwise. The ten validated GF2 runs above use a separately
frozen corrected executable; exploratory results are not substituted for them.

The comparison excludes HS/DS/GS inputs, samplers, full native pipeline-state
comparison, OM color/depth/stencil sample planes, hidden state and pixel-shader
execution results. In particular, sampled texture 422 is not the MSAA DSV
resource. Equal hashes of this selected set do not prove that all Draw inputs
or all pre-Draw samples are equal. Reports explicitly retain
`all_draw_inputs_proven_equal: false` and `acceptance_established: false`.

The next step is to retain the MSAA depth/stencil and relevant output samples
and compare fragment results across the two outcomes while preserving complete
replay. No capture component or driver is assigned fault from this evidence.

## Tools and boundaries

```
cmake --build --preset capture-full-release --target FloraReplayTexelProbe FloraInputCheckpointTests
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 inputs 5651 5651 3
FloraInputCheckpointTests NEW_REPORT_JSON
```

`inputs` extends the preceding single-event GPU-checkpoint mode. Unsupported
bound input resource types fail explicitly. All new code is development-only
under `tools/native`; Capture does not import or invoke it. The excluded replay
probe now links Qt Gui solely for lossless evidence images. Capture-only builds
do not find Qt or build these targets, and production packages are unchanged.

Raw evidence is retained under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-input-checkpoint-20261014`.
The [baseline](capture-gf2-input-checkpoint-baseline.json) records source and
binary identities, all fifteen input hashes, per-run outcomes, exact image
differences and the evidence manifest. Legacy migration parity is unchanged.
