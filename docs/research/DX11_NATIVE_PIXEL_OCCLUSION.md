# Native selected-region occlusion

This P5 building block executes the original selected draw in an independent
prefix replay and measures a D3D11 occlusion query. It intersects the selected
pixel with the original scissor and the selected output sample with the original
sample mask. Original shaders, resources, depth/stencil, blending and predication
remain active. No additional UAV slot is needed. The internal API is
`measureNativePixelOcclusion`; `FloraNativePixelOcclusionProbe` exercises it on
GPA files without loading original GPA or RenderDoc.

## Result boundaries

The result scope is `selected_region_replay`. It is an observation of a modified
draw, not proof that the original unrestricted draw had the same fragment
execution: cross-pixel UAV communication can change when the region is narrowed.
`original_fragment_eligibility` and `fragment_records` therefore remain null.
The returned view ID and native view descriptor identify its mip/array slice.
Currently the bound view must cover one layer; selecting within a multilayer
view, depth-only draws and UAV-only outputs require subsequent implementation.
These restrictions belong to the new internal diagnostic and do not replace
the existing public history backend.

The `passing_samples` value is the raw D3D11 occlusion count, not the number of
changed pixel values or final writes. Zero color write masks can still produce
positive counts. Forced early depth/stencil can count a sample subsequently
discarded by the shader. Forced sample counts refer to rasterizer samples, while
the selected sample/mask refers to output storage; forced-sample output masking
has specification-defined implementation freedom. See the primary
[D3D11.3 specification, sections 16.9 and 20.4.6](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).

The new replay instance owns its resources, counters and SO cursors. Modified
rasterizer/scissor/blend state is restored after the draw; failure or cancellation
discards the instance. This does not claim restoration of an externally supplied
live replay context, nor does it establish bounded prefix replay memory. Query
completion uses the existing cancellable fence/event or bounded polling helper.
Whole-process watchdogs remain necessary for unresponsive driver calls. Explicit
adapter selection is preserved and the actual adapter identity is reported.

## Validation and unresolved query interpretation

The new suite passes 51 cases on NVIDIA, Intel and WARP, including unchanged
writes, disabled draws, original scissor/depth/stencil/discard/sample restrictions,
overlap, two array slices with four MSAA samples, predication, invalid requests,
cancellation and retry. The independent owned fixture passes 72 selected-pixel
measurements (12 draws, two pixels, three adapters), with 576 rejected result
corruptions. Outside-scissor and zero-sample-mask draws return zero, unlike the
earlier original GPA diagnostic counter. No original GPA request is executed by
this validation.

The following are observations, not resolved shader/driver conformance findings.
A separate `NativeSample` full-draw query, without this new API's scissor or mask
modifications, produces the same values as the selected-region query:

| Fixture | WARP | NVIDIA | Intel |
| --- | ---: | ---: | ---: |
| Early depth, constant discard | 1 | 1 | 0 |
| Early depth, position-dependent discard | 1 | 1 | 0 |
| ForcedSampleCount 4, single output sample | 4 | 1 | 1 |

These controls share the existing frame replay but use its separate full-draw
query path; they are not an independent implementation of the entire pipeline.
They establish that regional clipping did not introduce these differences in
the one-pixel fixtures. They do not explain the hardware difference or certify
the query as an exact fragment-count oracle. Native descriptor snapshots confirm
ForcedSampleCount 4 and the original all-bits sample mask. Additional direct
device probes are required before such counts qualify fragment identity. The
new equivalence tests compare exact same-adapter raw query results; ordinary
coverage counts still use exact independently defined expectations.

Three real captures complete with no D3D debug errors: GF2 event 113 on NVIDIA
returns 1 (255 ms), BF1 event 2010 on Intel returns 2 (1,062 ms), and Helldivers
event 200 on WARP returns 1 (982 ms). These are single selected-region smoke
measurements, not full-history parity or stable performance benchmarks. Durations
include prefix replay and frame hashing; they are not GPU draw times.

Failed attempts remain in ignored evidence. The first overlap fixture supplied
an unsupported draw-argument override and still issued three vertices; the
corrected fixture contains six captured vertices and checks that fact before
measurement. Later tests initially assumed specification-derived early-depth
and forced-sample values were portable. The full-draw controls above exposed
their unresolved device differences. Both constant and conditional discard
remain tested, with exact raw-query equivalence; no preexisting golden or skip
condition was changed.

The final serial CTest run passes six executables in 62.97 seconds: source
boundaries, GPU completion, new occlusion, event history, depth/stencil and
predicate-query history. The Qt suites total 204 passed checks, with ten unchanged
optional predicate cases skipped because `artifacts/m2-predicate-creation-originals`
is absent. GUI, CLI, Worker and the probe build in Release. The probe's static
imports contain no original GPA or RenderDoc module; this is not a dynamic module
audit or the P8 clean-machine acceptance. The compact evidence ledger is
`dx11-native-pixel-occlusion-evidence.json`; failed and final reports are retained
separately under `artifacts/dx11-native/pixel-occlusion*`.

## Reproduction

Build `FloraNativePixelOcclusionTests` and `FloraNativePixelOcclusionProbe`, then
run the `native_pixel_occlusion` CTest serially. The synthetic tests require no
GPA installation, original capture or RenderDoc installation.

For the separately captured owned twelve-draw research fixture:

```powershell
python tools/validate_native_pixel_occlusion.py <fixture-directory> --probe build/vs2022/Release/FloraNativePixelOcclusionProbe.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out <new-directory>
```

This validator reuses fixture identity, event mapping and independent device
identity from the earlier research directory. It does not execute the original
player or use its diagnostic counts as expected values. Each subprocess has a
120-second limit. Raw evidence and captures remain outside Git.

## Remaining P5 work

Original fragment qualification, ordered fragment identities, shader outputs,
blend/depth/stencil transitions, failure attribution, multilayer/depth/UAV
selection and integration into the public history job remain open. The private
PS interpreter and original-input transport still need a verified join with
those fragments before public pixel debugging can claim parity. P7 removal and
P8 standalone acceptance remain gated by that work.
